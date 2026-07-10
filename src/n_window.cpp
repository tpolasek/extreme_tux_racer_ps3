/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types (SFML removal)

RenderWindow (X11 + GLX), Event dispatch, Joystick (/dev/input/js*).
---------------------------------------------------------------------*/
#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "n_window.h"
#include "n_draw.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <GL/glx.h>
#include <GL/gl.h>

#include <linux/joystick.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>
#include <string>

// ---------------------------------------------------------------------------
// Timing helper (frame limiting)
// ---------------------------------------------------------------------------
static long long nowUs() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<long long>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000;
}

// ---------------------------------------------------------------------------
// VideoMode
// ---------------------------------------------------------------------------
VideoMode VideoMode::getDesktopMode() noexcept {
	Display* d = XOpenDisplay(nullptr);
	if (!d) return VideoMode(1280, 720, 32);
	int screen = DefaultScreen(d);
	Screen* s = ScreenOfDisplay(d, screen);
	VideoMode vm(static_cast<unsigned int>(XWidthOfScreen(s)),
	             static_cast<unsigned int>(XHeightOfScreen(s)),
	             static_cast<unsigned int>(DefaultDepth(d, screen)));
	XCloseDisplay(d);
	return vm;
}

// ---------------------------------------------------------------------------
// Joystick (/dev/input/js*)
// ---------------------------------------------------------------------------
namespace {

struct JsDev {
	int  fd;
	int  axes;
	int  buttons;
	bool tried;
	JsDev() : fd(-1), axes(0), buttons(0), tried(false) {}
};

JsDev& jsDev(unsigned int i) {
	static JsDev devices[Joystick::Count];
	return devices[i];
}

void ensureJs(unsigned int i) {
	JsDev& d = jsDev(i);
	if (d.tried) return;
	d.tried = true;
	std::string path = "/dev/input/js" + std::to_string(i);
	int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
	if (fd < 0) return;
	d.fd = fd;
	unsigned char axes = 0, buttons = 0;
	ioctl(fd, JSIOCGAXES, &axes);
	ioctl(fd, JSIOCGBUTTONS, &buttons);
	d.axes = axes;
	d.buttons = buttons;
}

int axisEnumToIndex(Joystick::Axis a) {
	switch (a) {
		case Joystick::X:    return 0;
		case Joystick::Y:    return 1;
		case Joystick::Z:    return 2;
		case Joystick::R:    return 3;
		case Joystick::U:    return 4;
		case Joystick::V:    return 5;
		case Joystick::PovX: return 6;
		case Joystick::PovY: return 7;
	}
	return -1;
}

Joystick::Axis axisIndexToEnum(int index) {
	switch (index) {
		case 0: return Joystick::X;
		case 1: return Joystick::Y;
		case 2: return Joystick::Z;
		case 3: return Joystick::R;
		case 4: return Joystick::U;
		case 5: return Joystick::V;
		case 6: return Joystick::PovX;
		case 7: return Joystick::PovY;
		default: return Joystick::X;
	}
}

} // namespace

bool Joystick::isConnected(unsigned int joystick) {
	if (joystick >= Count) return false;
	ensureJs(joystick);
	return jsDev(joystick).fd >= 0;
}

unsigned int Joystick::getButtonCount(unsigned int joystick) {
	if (joystick >= Count) return 0;
	ensureJs(joystick);
	return static_cast<unsigned int>(jsDev(joystick).buttons);
}

unsigned int Joystick::getAxisCount(unsigned int joystick) {
	if (joystick >= Count) return 0;
	ensureJs(joystick);
	return static_cast<unsigned int>(jsDev(joystick).axes);
}

bool Joystick::hasAxis(unsigned int joystick, Axis axis) {
	if (joystick >= Count) return false;
	ensureJs(joystick);
	int idx = axisEnumToIndex(axis);
	return idx >= 0 && idx < jsDev(joystick).axes;
}

int Joystick::getAxisPosition(unsigned int joystick, Axis axis) {
	(void)joystick;
	(void)axis;
	return 0; // not tracked (the game consumes live axis events)
}

// ---------------------------------------------------------------------------
// RenderWindow
// ---------------------------------------------------------------------------
RenderWindow::RenderWindow()
	: display_(nullptr), window_(0), context_(nullptr), wmDelete_(0),
	  width_(0), height_(0), open_(false), framerateLimit_(0), lastSwapUs_(0) {}

RenderWindow::~RenderWindow() { close(); }

void RenderWindow::create(VideoMode mode, const std::string& title, Uint32 style, const ContextSettings& settings) {
	close();

	Display* dpy = XOpenDisplay(nullptr);
	if (!dpy) return;
	int screen = DefaultScreen(dpy);

	int attribs[] = {
		GLX_RENDER_TYPE,   GLX_RGBA_BIT,
		GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
		GLX_X_RENDERABLE,  True,
		GLX_DOUBLEBUFFER,  True,
		GLX_RED_SIZE,      8,
		GLX_GREEN_SIZE,    8,
		GLX_BLUE_SIZE,     8,
		GLX_ALPHA_SIZE,    8,
		GLX_DEPTH_SIZE,    static_cast<int>(settings.depthBits),
		GLX_STENCIL_SIZE,  static_cast<int>(settings.stencilBits),
		0
	};
	int fbCount = 0;
	GLXFBConfig* fbc = glXChooseFBConfig(dpy, screen, attribs, &fbCount);
	if (!fbc || fbCount == 0) { if (fbc) XFree(fbc); XCloseDisplay(dpy); return; }
	GLXFBConfig fb = fbc[0];

	XVisualInfo* vi = glXGetVisualFromFBConfig(dpy, fb);
	if (!vi) { XFree(fbc); XCloseDisplay(dpy); return; }

	GLXContext ctx = glXCreateNewContext(dpy, fb, GLX_RGBA_TYPE, nullptr, True);
	if (!ctx) { XFree(vi); XFree(fbc); XCloseDisplay(dpy); return; }

	Colormap cmap = XCreateColormap(dpy, RootWindow(dpy, screen), vi->visual, AllocNone);
	XSetWindowAttributes swa;
	swa.colormap = cmap;
	swa.border_pixel = 0;
	swa.event_mask = ExposureMask | StructureNotifyMask;
	unsigned long valuemask = CWColormap | CWBorderPixel | CWEventMask;

	Window win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, mode.width, mode.height, 0,
	                           vi->depth, InputOutput, vi->visual, valuemask, &swa);

	XStoreName(dpy, win, title.c_str());

	Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &wmDelete, 1);

	if (style & Style::Fullscreen) {
		Atom wmState = XInternAtom(dpy, "_NET_WM_STATE", False);
		Atom full    = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
		XChangeProperty(dpy, win, wmState, XA_ATOM, 32, PropModeReplace,
		                reinterpret_cast<unsigned char*>(&full), 1);
	}

	XMapWindow(dpy, win);
	glXMakeCurrent(dpy, win, ctx);

	display_  = dpy;
	window_   = static_cast<std::uintptr_t>(win);
	context_  = ctx;
	wmDelete_ = static_cast<unsigned long>(wmDelete);
	width_    = mode.width;
	height_   = mode.height;
	open_     = true;
	lastSwapUs_ = nowUs();

	XFree(vi);
	XFree(fbc);
}

void RenderWindow::close() {
	if (!open_) return;
	Display* dpy = static_cast<Display*>(display_);
	GLXContext ctx = static_cast<GLXContext>(context_);
	glXMakeCurrent(dpy, 0, nullptr);
	glXDestroyContext(dpy, ctx);
	XDestroyWindow(dpy, static_cast<Window>(window_));
	XCloseDisplay(dpy);
	display_ = nullptr;
	window_  = 0;
	context_ = nullptr;
	open_    = false;
}

bool RenderWindow::isOpen() const noexcept { return open_; }

void RenderWindow::display() {
	Display* dpy = static_cast<Display*>(display_);
	if (framerateLimit_ > 0) {
		long long target = 1000000LL / static_cast<long long>(framerateLimit_);
		long long elapsed = nowUs() - lastSwapUs_;
		if (elapsed < target) {
			timespec ts;
			ts.tv_sec = 0;
			ts.tv_nsec = (target - elapsed) * 1000;
			nanosleep(&ts, nullptr);
		}
	}
	glXSwapBuffers(dpy, static_cast<Window>(window_));
	lastSwapUs_ = nowUs();
}

void RenderWindow::clear(const Color& color) {
	glClearColor(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void RenderWindow::draw(const Drawable2D& drawable, const RenderStates&) {
	drawable.draw();
}

bool RenderWindow::pollEvent(Event& event) {
	Display* dpy = static_cast<Display*>(display_);
	if (!dpy) return false;

	// 1) X11 events (window close).
	while (XPending(dpy) > 0) {
		XEvent xe;
		XNextEvent(dpy, &xe);
		if (xe.type == ClientMessage) {
			if (static_cast<unsigned long>(xe.xclient.data.l[0]) == wmDelete_) {
				event.type = Event::Closed;
				return true;
			}
		}
	}

	// 2) Joystick events (/dev/input/js*).
	for (unsigned int i = 0; i < Joystick::Count; ++i) {
		ensureJs(i);
		JsDev& d = jsDev(i);
		if (d.fd < 0) continue;

		js_event ev;
		ssize_t n;
		while ((n = ::read(d.fd, &ev, sizeof(ev))) == sizeof(ev)) {
			if (ev.type & JS_EVENT_INIT) continue; // skip initial state

			if (ev.type & JS_EVENT_AXIS) {
				float pos = static_cast<float>(ev.value) / 327.67f;
				if (pos < -100.0f) pos = -100.0f;
				if (pos >  100.0f) pos =  100.0f;
				event.type = Event::JoystickMoved;
				event.joystickMove.joystickId = i;
				event.joystickMove.axis = axisIndexToEnum(ev.number);
				event.joystickMove.position = pos;
				return true;
			} else if (ev.type & JS_EVENT_BUTTON) {
				event.type = ev.value ? Event::JoystickButtonPressed : Event::JoystickButtonReleased;
				event.joystickButton.joystickId = i;
				event.joystickButton.button = static_cast<unsigned int>(ev.number);
				return true;
			}
		}
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
