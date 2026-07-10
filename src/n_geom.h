/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types

Geometry POD types + Duration (named to dodge X11's global
`typedef ... Time`).
---------------------------------------------------------------------*/
#ifndef N_GEOM_H
#define N_GEOM_H

#include <cstdint>

template<typename T>
struct Vector2 {
	T x, y;
	constexpr Vector2() noexcept : x(0), y(0) {}
	constexpr Vector2(T X, T Y) noexcept : x(X), y(Y) {}
	template<typename U>
	constexpr explicit Vector2(const Vector2<U>& v) noexcept : x(static_cast<T>(v.x)), y(static_cast<T>(v.y)) {}
};
using Vector2i = Vector2<int>;
using Vector2u = Vector2<unsigned int>;
using Vector2f = Vector2<float>;

template<typename T>
struct Rect {
	T left, top, width, height;
	constexpr Rect() noexcept : left(0), top(0), width(0), height(0) {}
	constexpr Rect(T l, T t, T w, T h) noexcept : left(l), top(t), width(w), height(h) {}
	constexpr bool contains(T x, T y) const noexcept {
		return x >= left && x < left + width && y >= top && y < top + height;
	}
	constexpr bool intersects(const Rect<T>& r) const noexcept {
		return !(left + width <= r.left || r.left + r.width <= left ||
		         top + height <= r.top || r.top + r.height <= top);
	}
};
using IntRect   = Rect<int>;
using FloatRect = Rect<float>;

// Duration avoids the name `Time` (X11 typedefs it globally).
class Duration {
	int64_t us_;
public:
	constexpr Duration() noexcept : us_(0) {}
	constexpr explicit Duration(int64_t us) noexcept : us_(us) {}
	constexpr float    asSeconds() const noexcept      { return static_cast<float>(us_) / 1000000.0f; }
	constexpr int32_t  asMilliseconds() const noexcept { return static_cast<int32_t>(us_ / 1000); }
	constexpr int64_t  asMicroseconds() const noexcept { return us_; }
	static constexpr Duration seconds(float s) noexcept       { return Duration(static_cast<int64_t>(s * 1000000.0f)); }
	static constexpr Duration milliseconds(int32_t ms) noexcept { return Duration(static_cast<int64_t>(ms) * 1000); }
};

inline bool operator==(const Duration& l, const Duration& r) noexcept { return l.asMicroseconds() == r.asMicroseconds(); }
inline bool operator!=(const Duration& l, const Duration& r) noexcept { return !(l == r); }
inline bool operator< (const Duration& l, const Duration& r) noexcept { return l.asMicroseconds() <  r.asMicroseconds(); }
inline bool operator> (const Duration& l, const Duration& r) noexcept { return l.asMicroseconds() >  r.asMicroseconds(); }
inline Duration operator-(const Duration& l, const Duration& r) noexcept { return Duration(l.asMicroseconds() - r.asMicroseconds()); }
inline Duration operator+(const Duration& l, const Duration& r) noexcept { return Duration(l.asMicroseconds() + r.asMicroseconds()); }

#endif // N_GEOM_H
