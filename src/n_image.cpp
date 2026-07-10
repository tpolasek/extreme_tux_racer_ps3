/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types (SFML removal)

Image implementation - libpng decode/encode to RGBA8.
---------------------------------------------------------------------*/
#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "n_image.h"

#include <png.h>
#include <cstdio>
#include <cstring>

Image::Image() : width_(0), height_(0) {}

void Image::create(unsigned int w, unsigned int h, const Color& fill) {
	width_ = w;
	height_ = h;
	pixels_.assign(static_cast<std::size_t>(w) * h * 4, 0);
	for (std::size_t i = 0; i < pixels_.size(); i += 4) {
		pixels_[i + 0] = fill.r;
		pixels_[i + 1] = fill.g;
		pixels_[i + 2] = fill.b;
		pixels_[i + 3] = fill.a;
	}
}

void Image::create(unsigned int w, unsigned int h, const Uint8* rgba) {
	width_ = w;
	height_ = h;
	pixels_.resize(static_cast<std::size_t>(w) * h * 4);
	if (rgba) std::memcpy(pixels_.data(), rgba, pixels_.size());
	else     std::memset(pixels_.data(), 0, pixels_.size());
}

bool Image::loadFromFile(const std::string& filename) {
	FILE* fp = std::fopen(filename.c_str(), "rb");
	if (!fp) return false;

	unsigned char header[8];
	if (std::fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8)) {
		std::fclose(fp);
		return false;
	}

	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (!png) { std::fclose(fp); return false; }
	png_infop info = png_create_info_struct(png);
	if (!info) { png_destroy_read_struct(&png, nullptr, nullptr); std::fclose(fp); return false; }

	if (setjmp(png_jmpbuf(png))) {
		png_destroy_read_struct(&png, &info, nullptr);
		std::fclose(fp);
		return false;
	}

	png_init_io(png, fp);
	png_set_sig_bytes(png, 8);
	png_read_info(png, info);

	png_uint_32 w, h;
	int bitDepth, colorType, interlace, compression, filter;
	png_get_IHDR(png, info, &w, &h, &bitDepth, &colorType, &interlace, &compression, &filter);

	// Normalize everything to 8-bit RGBA.
	if (bitDepth == 16) png_set_strip_16(png);
	if (colorType == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
	if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) png_set_expand_gray_1_2_4_to_8(png);
	if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
	if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
	png_set_filler(png, 0xFF, PNG_FILLER_AFTER);   // add opaque alpha if missing
	png_set_packing(png);
	png_read_update_info(png, info);

	const std::size_t rowBytes = png_get_rowbytes(png, info);
	pixels_.resize(rowBytes * h);
	std::vector<png_bytep> rows(h);
	for (png_uint_32 y = 0; y < h; y++)
		rows[y] = pixels_.data() + y * rowBytes;

	png_read_image(png, rows.data());
	png_read_end(png, nullptr);

	png_destroy_read_struct(&png, &info, nullptr);
	std::fclose(fp);

	width_ = w;
	height_ = h;
	return true;
}

bool Image::saveToFile(const std::string& filename) const {
	if (width_ == 0 || height_ == 0) return false;

	FILE* fp = std::fopen(filename.c_str(), "wb");
	if (!fp) return false;

	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (!png) { std::fclose(fp); return false; }
	png_infop info = png_create_info_struct(png);
	if (!info) { png_destroy_write_struct(&png, nullptr); std::fclose(fp); return false; }

	if (setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		std::fclose(fp);
		return false;
	}

	png_init_io(png, fp);
	png_set_IHDR(png, info, width_, height_, 8, PNG_COLOR_TYPE_RGB_ALPHA,
	             PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);

	std::vector<png_bytep> rows(height_);
	const std::size_t rowBytes = static_cast<std::size_t>(width_) * 4;
	for (unsigned int y = 0; y < height_; y++)
		rows[y] = const_cast<png_bytep>(pixels_.data() + y * rowBytes);
	png_write_image(png, rows.data());
	png_write_end(png, nullptr);

	png_destroy_write_struct(&png, &info);
	std::fclose(fp);
	return true;
}

void Image::flipVertically() {
	const std::size_t rowBytes = static_cast<std::size_t>(width_) * 4;
	std::vector<Uint8> tmp(rowBytes);
	for (unsigned int y = 0; y < height_ / 2; y++) {
		Uint8* top = pixels_.data() + y * rowBytes;
		Uint8* bot = pixels_.data() + (height_ - 1 - y) * rowBytes;
		std::memcpy(tmp.data(), top, rowBytes);
		std::memcpy(top, bot, rowBytes);
		std::memcpy(bot, tmp.data(), rowBytes);
	}
}

void Image::setPixel(unsigned int x, unsigned int y, const Color& c) {
	if (x >= width_ || y >= height_) return;
	Uint8* p = pixels_.data() + (static_cast<std::size_t>(y) * width_ + x) * 4;
	p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
}

Color Image::getPixel(unsigned int x, unsigned int y) const {
	if (x >= width_ || y >= height_) return Color();
	const Uint8* p = pixels_.data() + (static_cast<std::size_t>(y) * width_ + x) * 4;
	return Color(p[0], p[1], p[2], p[3]);
}
