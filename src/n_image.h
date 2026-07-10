/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types

Image - raw RGBA pixel buffer with PNG load/save.
libpng is used internally (header has no libpng dependency).
---------------------------------------------------------------------*/
#ifndef N_IMAGE_H
#define N_IMAGE_H

#include "n_color.h"
#include "n_geom.h"

#include <string>
#include <vector>

class Image {
	unsigned int width_, height_;
	std::vector<Uint8> pixels_; // RGBA8, top-left origin
public:
	Image();

	void create(unsigned int w, unsigned int h, const Color& fill = Color(0, 0, 0, 0));
	void create(unsigned int w, unsigned int h, const Uint8* rgba); // copy RGBA8 buffer

	bool loadFromFile(const std::string& filename);
	bool saveToFile(const std::string& filename) const;

	Vector2u getSize() const noexcept { return Vector2u(width_, height_); }
	unsigned int getWidth() const noexcept { return width_; }
	unsigned int getHeight() const noexcept { return height_; }

	const Uint8* getPixelsPtr() const noexcept { return pixels_.data(); }

	void flipVertically();
	void setPixel(unsigned int x, unsigned int y, const Color& c);
	Color getPixel(unsigned int x, unsigned int y) const;
};

#endif // N_IMAGE_H
