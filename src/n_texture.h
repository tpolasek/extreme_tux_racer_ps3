/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types

Texture - GL 2D texture backed by libpng loading.
---------------------------------------------------------------------*/
#ifndef N_TEXTURE_H
#define N_TEXTURE_H

#include "n_color.h"
#include "n_geom.h"

#include <GL/gl.h>
#include <string>

class Image;

class Texture {
	unsigned int width_, height_;
	GLuint texture_;
	bool smooth_, repeated_;
	bool hasTexture_;
public:
	Texture();
	~Texture();
	Texture(const Texture&) = delete;
	Texture& operator=(const Texture&) = delete;

	bool loadFromFile(const std::string& filename);
	bool loadFromImage(const Image& image);

	void setSmooth(bool smooth);
	void setRepeated(bool repeated);

	static void bind(const Texture* texture);

	Vector2u getSize() const noexcept { return Vector2u(width_, height_); }
	unsigned int getWidth() const noexcept { return width_; }
	unsigned int getHeight() const noexcept { return height_; }
	GLuint getNativeHandle() const noexcept { return texture_; }

private:
	void release();
	void applyParams();
	void upload(const Uint8* rgba, unsigned int w, unsigned int h);
};

#endif // N_TEXTURE_H
