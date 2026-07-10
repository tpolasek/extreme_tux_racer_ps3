/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types (SFML removal)

Color POD type + integer typedefs. Replaces sf::Color / sf::Uint8/32.
---------------------------------------------------------------------*/
#ifndef N_COLOR_H
#define N_COLOR_H

#include <cstdint>

using Int8   = std::int8_t;
using Int16  = std::int16_t;
using Int32  = std::int32_t;
using Int64  = std::int64_t;
using Uint8  = std::uint8_t;
using Uint16 = std::uint16_t;
using Uint32 = std::uint32_t;
using Uint64 = std::uint64_t;

struct Color {
	Uint8 r, g, b, a;

	constexpr Color() noexcept : r(0), g(0), b(0), a(255) {}
	constexpr Color(Uint8 R, Uint8 G, Uint8 B, Uint8 A = 255) noexcept : r(R), g(G), b(B), a(A) {}

#if defined(SFML_GRAPHICS_HPP)
	// Coexistence bridge: allows the migrated native Color to flow into
	// not-yet-migrated SFML APIs (sf::Text::setFillColor, sf::RenderWindow::clear,
	// ...). This block disappears once SFML is removed.
	Color(const sf::Color& c) noexcept : r(c.r), g(c.g), b(c.b), a(c.a) {}
	operator sf::Color() const noexcept { return sf::Color(r, g, b, a); }
#endif

	static const Color Transparent; // 0,0,0,0
	static const Color White;       // 255,255,255,255
	static const Color Black;       // 0,0,0,255
	static const Color Red;         // 255,0,0,255
	static const Color Green;       // 0,255,0,255
	static const Color Blue;        // 0,0,255,255
	static const Color Yellow;      // 255,255,0,255
	static const Color Magenta;     // 255,0,255,255
	static const Color Cyan;        // 0,255,255,255
};

inline bool operator==(const Color& l, const Color& r) noexcept { return l.r == r.r && l.g == r.g && l.b == r.b && l.a == r.a; }
inline bool operator!=(const Color& l, const Color& r) noexcept { return !(l == r); }

#endif // N_COLOR_H
