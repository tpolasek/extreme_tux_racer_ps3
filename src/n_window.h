/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types (SFML removal)

Window / event / input: RenderWindow (X11+GLX), Event, Joystick
(/dev/input/js*), Keyboard, VideoMode, ContextSettings, Style.

X11 is kept out of this header (opaque handles) so it does not
propagate through bh.h.
---------------------------------------------------------------------*/
#ifndef N_WINDOW_H
#define N_WINDOW_H

#include "n_color.h"
#include "n_geom.h"
#include "n_draw.h"

#include <cstdint>
#include <string>

// Window styles (SFML-compatible names). Note: bare `None` is an X11
// macro, so it is intentionally omitted here.
struct Style {
	static constexpr Uint32 Titlebar   = 1;
	static constexpr Uint32 Close      = 2;
	static constexpr Uint32 Resize     = 4;
	static constexpr Uint32 Fullscreen = 8;
	static constexpr Uint32 Default    = Titlebar | Resize | Close;
};

struct VideoMode {
	unsigned int width;
	unsigned int height;
	unsigned int bitsPerPixel;
	VideoMode() noexcept : width(0), height(0), bitsPerPixel(32) {}
	VideoMode(unsigned int w, unsigned int h, unsigned int bpp = 32) noexcept
		: width(w), height(h), bitsPerPixel(bpp) {}
	static VideoMode getDesktopMode() noexcept;
};

struct ContextSettings {
	unsigned int depthBits;
	unsigned int stencilBits;
	unsigned int antialiasingLevel;
	unsigned int majorVersion;
	unsigned int minorVersion;
	ContextSettings() noexcept
		: depthBits(24), stencilBits(0), antialiasingLevel(0), majorVersion(1), minorVersion(2) {}
	ContextSettings(unsigned int depth, unsigned int stencil, unsigned int aa,
	                unsigned int major, unsigned int minor) noexcept
		: depthBits(depth), stencilBits(stencil), antialiasingLevel(aa),
		  majorVersion(major), minorVersion(minor) {}
};

// Joystick API mirrors sf::Joystick (static methods, nested Axis enum).
class Joystick {
public:
	enum Axis { X, Y, Z, R, U, V, PovX, PovY };

	static constexpr unsigned int Count = 8;

	static bool         isConnected(unsigned int joystick);
	static unsigned int getButtonCount(unsigned int joystick);
	static unsigned int getAxisCount(unsigned int joystick);
	static bool         hasAxis(unsigned int joystick, Axis axis);
	static int          getAxisPosition(unsigned int joystick, Axis axis); // -100..100
	static void         update() {} // no-op (events polled directly)
};

// Keyboard mirrors sf::Keyboard (dev tools only; not in the main game loop).
class Keyboard {
public:
	enum Key {
		Unknown = -1,
		A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
		Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
		Escape,
		LControl, RControl, LShift, RShift, LAlt, RAlt, LSystem, RSystem, Menu,
		LBracket, RBracket, Semicolon, Comma, Period, Quote, Slash, Backslash, Tilde, Equal, Dash, Space, Return, Backspace, Tab,
		PageUp, PageDown, End, Home, Insert, Delete,
		Add, Subtract, Multiply, Divide, Left, Right, Up, Down,
		Numpad0, Numpad1, Numpad2, Numpad3, Numpad4, Numpad5, Numpad6, Numpad7, Numpad8, Numpad9,
		F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12, F13, F14, F15,
		Pause, KeyCount
	};
};

// Event mirrors the subset of sf::Event used by the game.
class Event {
public:
	enum EventType {
		Closed,
		Resized,
		JoystickMoved,
		JoystickButtonPressed,
		JoystickButtonReleased
	};

	struct JoystickMoveEvent {
		unsigned int   joystickId;
		Joystick::Axis axis;
		float          position;
	};
	struct JoystickButtonEvent {
		unsigned int joystickId;
		unsigned int button;
	};

	EventType            type;
	JoystickMoveEvent    joystickMove;
	JoystickButtonEvent  joystickButton;
};

// RenderWindow wraps an X11 window + GLX context.
class RenderWindow {
public:
	RenderWindow();
	~RenderWindow();

	RenderWindow(const RenderWindow&) = delete;
	RenderWindow& operator=(const RenderWindow&) = delete;

	void create(VideoMode mode, const std::string& title, Uint32 style, const ContextSettings& settings);
	void close();
	bool isOpen() const noexcept;

	void display();
	void clear(const Color& color = Color(0, 0, 0, 255));

	void draw(const Drawable2D& drawable, const RenderStates& states = RenderStates::Default);

	bool pollEvent(Event& event);

	void pushGLStates();
	void popGLStates();

	Vector2u getSize() const noexcept { return Vector2u(width_, height_); }
	void setFramerateLimit(unsigned int limit) noexcept { framerateLimit_ = limit; }
	std::uintptr_t getSystemHandle() const noexcept { return window_; }

private:
	// Opaque platform handles (X11/G types hidden in the .cpp).
	void*          display_;
	std::uintptr_t window_;
	void*          context_;
	unsigned long  wmDelete_;
	unsigned int   width_;
	unsigned int   height_;
	bool           open_;
	unsigned int   framerateLimit_;
	long long      lastSwapUs_;
};

#endif // N_WINDOW_H
