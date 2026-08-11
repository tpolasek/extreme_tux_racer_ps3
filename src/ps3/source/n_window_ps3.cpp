/* ETR PS3 backend for the n_window abstraction (replaces n_window.cpp).
 *
 * Implements RenderWindow / Joystick / Event / VideoMode from n_window.h on
 * top of PSL1GHT: RSX via rsxutil (init_screen/flip), the GL shim for clears,
 * io/pad.h for DualShock input, and sysutil for the XMB quit signal.
 *
 * The contract (n_window.h) is platform-neutral and unchanged; only this .cpp
 * differs from the Linux (X11+GLX) backend.
 */
#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "n_window.h"
#include "n_draw.h"
#include "GL/gl.h"

#include <rsx/rsx.h>
#include <io/pad.h>
#include <sysutil/sysutil.h>
#include <sys/thread.h>
#include <sys/process.h>
#include <sys/systime.h>
#include <sys/tty.h>
#include <ppu-types.h>
#include <malloc.h>
#include <unistd.h>
#include <string.h>

#include "rsxutil.h"
#include "ps3_gl_internal.h"
#include "ps3_tty.h"
#include "ps3_log.h"

/* host buffer handed to the RSX (128 MiB, 1 MiB aligned) */
#define PS3_HOST_SIZE     (128 * 1024 * 1024)
#define PS3_HOST_ALIGN    (1024 * 1024)

/* ------------------------------------------------------------------ timing */
static u64 nowUs() {
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return sec * 1000000ULL + nsec / 1000ULL;
}

/* ------------------------------------------------------------------ exit */
static volatile int g_exitRequested = 0;
static void sysutilExitCb(u64 status, u64 /*param*/, void* /*usr*/) {
	if (status == SYSUTIL_EXIT_GAME) g_exitRequested = 1;
}
static void registerExitCallback() {
	static int done = 0;
	if (done) return;
	done = 1;
	sysUtilRegisterCallback(0, sysutilExitCb, NULL);
}

/* ------------------------------------------------------------------ pad
 * Button index mapping (game logic):
 *   0=Cross(confirm) 1=Circle(back) 2=Square 3=Triangle
 *   4=L1 5=R1 6=L2 7=Start(racing: exit) 8=R2
 *
 * padData (io/pad.h) exposes the DualShock state as named 1-bit bitfields
 * (BTN_CROSS, BTN_CIRCLE, ...) and 16-bit analog fields (ANA_L_H, ANA_L_V).
 * Per the ioPadGetData contract: "The padData structure is only filled if
 * there is a change in the input since the last call. If there is no change,
 * the structure is zero filled. If the member 'len' is zero, it indicates
 * that there was no new input." We therefore cache the last non-empty report
 * and sample against that, so held buttons don't generate spurious releases.
 */
/* Map game button index -> DualShock named bit-field member. padData exposes
 * these as 1-bit bitfields, so we can't take their address (no pointer-to-
 * member); a switch reads each by name. Returns 0/1. */
#define BTN_MAP_COUNT 9
static u8 readPadButton(const padData &p, unsigned int button) {
	switch (button) {
		case 0: return (u8)p.BTN_CROSS;
		case 1: return (u8)p.BTN_CIRCLE;
		case 2: return (u8)p.BTN_SQUARE;
		case 3: return (u8)p.BTN_TRIANGLE;
		case 4: return (u8)p.BTN_L1;
		case 5: return (u8)p.BTN_R1;
		case 6: return (u8)p.BTN_L2;
		case 7: return (u8)p.BTN_START;
		case 8: return (u8)p.BTN_R2;
		default: return 0;
	}
}

#define ANALOG_DEADZONE 30   /* out of 128 */

static inline float axisFromByte(u8 v) {
	int c = (int)v - 128;
	if (c > -ANALOG_DEADZONE && c < ANALOG_DEADZONE) c = 0;
	return (float)c * (100.0f / 128.0f);
}

/* pending event queue (filled by samplePad, drained by pollEvent) */
#define EVT_QUEUE 64
static Event  g_evtQueue[EVT_QUEUE];
static int    g_evtCount = 0;
static int    g_evtHead  = 0;

static void pushEvent(const Event &e) {
	if (g_evtCount >= EVT_QUEUE) return; /* overflow: drop */
	int idx = (g_evtHead + g_evtCount) % EVT_QUEUE;
	g_evtQueue[idx] = e;
	g_evtCount++;
}

static int popEvent(Event &out) {
	if (g_evtCount == 0) return 0;
	out = g_evtQueue[g_evtHead];
	g_evtHead = (g_evtHead + 1) % EVT_QUEUE;
	g_evtCount--;
	return 1;
}

/* previous state for edge detection */
static u8    s_prevBtn[BTN_MAP_COUNT]    = {0};
static float s_prevLX = 0.0f, s_prevLY = 0.0f;
static u8    s_prevDpadU = 0, s_prevDpadD = 0, s_prevDpadL = 0, s_prevDpadR = 0;
static int   s_padInited = 0;

/* cached last good pad report (see note above) */
static padData s_pad;
static int     s_padValid = 0;

/* Read pad 0 into s_pad; returns 1 if s_pad now reflects the current
 * effective pad state (either a fresh report or the previously cached one). */
static int readPad() {
	if (!s_padInited) { ioPadInit(MAX_PADS); s_padInited = 1; }

	padInfo2 info;
	ioPadGetInfo2(&info);
	if (!(info.port_status[0] & 1)) { s_padValid = 0; return 0; }

	padData fresh;
	memset(&fresh, 0, sizeof(fresh));
	if (ioPadGetData(0, &fresh) != 0) { s_padValid = 0; return 0; }

	if (fresh.len != 0) {
		s_pad = fresh;
		s_padValid = 1;
	}
	return s_padValid;
}

static void sampleAxes(unsigned int joyId) {
	if (!s_padValid) return;

	/* analog stick -> X / Y */
	float lx = axisFromByte((u8)s_pad.ANA_L_H);
	float ly = axisFromByte((u8)s_pad.ANA_L_V);
	if (lx != s_prevLX) {
		Event e; e.type = Event::JoystickMoved;
		e.joystickMove.joystickId = joyId; e.joystickMove.axis = Joystick::X; e.joystickMove.position = lx;
		pushEvent(e); s_prevLX = lx;
	}
	if (ly != s_prevLY) {
		Event e; e.type = Event::JoystickMoved;
		e.joystickMove.joystickId = joyId; e.joystickMove.axis = Joystick::Y; e.joystickMove.position = ly;
		pushEvent(e); s_prevLY = ly;
	}

	/* D-pad -> synthetic X/Y axis events (menu friendliness) */
	u8 du = (u8)s_pad.BTN_UP, dd = (u8)s_pad.BTN_DOWN;
	u8 dl = (u8)s_pad.BTN_LEFT, dr = (u8)s_pad.BTN_RIGHT;
	float dpadY = du ? -100.0f : (dd ? 100.0f : 0.0f);
	float dpadX = dl ? -100.0f : (dr ? 100.0f : 0.0f);
	float prevDpadY = s_prevDpadU ? -100.0f : (s_prevDpadD ? 100.0f : 0.0f);
	float prevDpadX = s_prevDpadL ? -100.0f : (s_prevDpadR ? 100.0f : 0.0f);
	if (dpadY != prevDpadY) {
		Event e; e.type = Event::JoystickMoved;
		e.joystickMove.joystickId = joyId; e.joystickMove.axis = Joystick::Y; e.joystickMove.position = dpadY;
		pushEvent(e);
	}
	if (dpadX != prevDpadX) {
		Event e; e.type = Event::JoystickMoved;
		e.joystickMove.joystickId = joyId; e.joystickMove.axis = Joystick::X; e.joystickMove.position = dpadX;
		pushEvent(e);
	}
	s_prevDpadU = du; s_prevDpadD = dd; s_prevDpadL = dl; s_prevDpadR = dr;
}

static void sampleButtons(unsigned int joyId) {
	if (!s_padValid) return;
	for (unsigned int i = 0; i < BTN_MAP_COUNT; i++) {
		u8 cur = readPadButton(s_pad, i);
		if (cur != s_prevBtn[i]) {
			Event e;
			e.type = cur ? Event::JoystickButtonPressed : Event::JoystickButtonReleased;
			e.joystickButton.joystickId = joyId;
			e.joystickButton.button = i;
			pushEvent(e);
			s_prevBtn[i] = cur;
		}
	}
}

/* =====================================================================
 * VideoMode
 * ===================================================================== */
VideoMode VideoMode::getDesktopMode() noexcept {
	return VideoMode(display_width, display_height, 32);
}

/* =====================================================================
 * Joystick
 * ===================================================================== */
bool Joystick::isConnected(unsigned int) {
	padInfo2 info;
	ioPadGetInfo2(&info);
	return (info.port_status[0] & 1) != 0;
}
unsigned int Joystick::getButtonCount(unsigned int) { return BTN_MAP_COUNT; }
unsigned int Joystick::getAxisCount(unsigned int)   { return 2; }
bool Joystick::hasAxis(unsigned int, Axis a)        { return a == X || a == Y; }
int  Joystick::getAxisPosition(unsigned int, Axis)  { return 0; }

/* =====================================================================
 * RenderWindow
 * ===================================================================== */
RenderWindow::RenderWindow()
	: display_(nullptr), window_(0), context_(nullptr), wmDelete_(0),
	  width_(0), height_(0), open_(false), framerateLimit_(0), lastSwapUs_(0) {}

RenderWindow::~RenderWindow() { close(); }

void RenderWindow::create(VideoMode /*mode*/, const std::string& /*title*/, Uint32 /*style*/, const ContextSettings& /*settings*/) {
	close();
	sysTtyTrace("[etr] RenderWindow::create: init RSX\n");

	void *host = memalign(PS3_HOST_ALIGN, PS3_HOST_SIZE);
	if (!host) { sysTtyTrace("[etr] create: host memalign FAILED\n"); return; }
	init_screen(host, PS3_HOST_SIZE);

	width_  = display_width;
	height_ = display_height;
	open_   = true;
	lastSwapUs_ = (long long)nowUs();

	/* load shader + GL state, set initial render target */
	ps3_gl_init();

	registerExitCallback();
	sysTtyTrace("[etr] RenderWindow::create: done\n");
}

void RenderWindow::close() {
	if (!open_) return;
	open_ = false;
	/* rsxFinish left to program exit callback */
}

bool RenderWindow::isOpen() const noexcept { return open_; }

void RenderWindow::display() {
	if (framerateLimit_ > 0) {
		long long target = 1000000LL / (long long)framerateLimit_;
		long long elapsed = (long long)nowUs() - lastSwapUs_;
		if (elapsed < target) {
			TIMER_START("FREE_TIME");
			usleep((useconds_t)(target - elapsed));
			TIMER_END("FREE_TIME");
		}
	}
	TIMER_START("FLIP");
	ps3_gl_flush_pending();
	flip();
	ps3_gl_invalidate_rsx_state();
	TIMER_END("FLIP");
	lastSwapUs_ = (long long)nowUs();
}

void RenderWindow::clear(const Color& color) {
	glClearColor(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void RenderWindow::draw(const Drawable2D& drawable, const RenderStates&) {
	drawable.draw();
}

bool RenderWindow::pollEvent(Event& event) {
	sysUtilCheckCallback();

	if (g_evtCount == 0) {
		if (readPad()) {
			sampleAxes(0);
			sampleButtons(0);
		}
	}

	if (popEvent(event)) return true;

	if (g_exitRequested) {
		g_exitRequested = 0;
		event.type = Event::Closed;
		return true;
	}
	return false;
}

void RenderWindow::pushGLStates() {
	glPushAttrib(GL_ALL_ATTRIB_BITS);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
}

void RenderWindow::popGLStates() {
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glPopAttrib();
}
