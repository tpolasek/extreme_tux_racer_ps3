/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types

Texture implementation - libpng decode (via Image) + glTexImage2D upload.
---------------------------------------------------------------------*/
#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "n_texture.h"
#include "n_image.h"

Texture::Texture()
	: width_(0), height_(0), texture_(0), smooth_(true), repeated_(false), hasTexture_(false) {}

Texture::~Texture() { release(); }

void Texture::release() {
	if (texture_) {
		glDeleteTextures(1, &texture_);
		texture_ = 0;
	}
	hasTexture_ = false;
}

bool Texture::loadFromFile(const std::string& filename) {
	Image img;
	if (!img.loadFromFile(filename)) return false;
	return loadFromImage(img);
}

bool Texture::loadFromImage(const Image& image) {
	upload(image.getPixelsPtr(), image.getWidth(), image.getHeight());
	width_ = image.getWidth();
	height_ = image.getHeight();
	return hasTexture_;
}

void Texture::upload(const Uint8* rgba, unsigned int w, unsigned int h) {
	if (!texture_) glGenTextures(1, &texture_);
	glBindTexture(GL_TEXTURE_2D, texture_);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	applyParams();
	hasTexture_ = true;
}

void Texture::applyParams() {
	glBindTexture(GL_TEXTURE_2D, texture_);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth_ ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth_ ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeated_ ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeated_ ? GL_REPEAT : GL_CLAMP_TO_EDGE);
}

void Texture::setSmooth(bool smooth) {
	if (smooth_ != smooth) {
		smooth_ = smooth;
		if (hasTexture_) applyParams();
	}
}

void Texture::setRepeated(bool repeated) {
	if (repeated_ != repeated) {
		repeated_ = repeated;
		if (hasTexture_) applyParams();
	}
}

void Texture::bind(const Texture* texture) {
	glBindTexture(GL_TEXTURE_2D, texture ? texture->texture_ : 0);
}
