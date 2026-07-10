/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types (SFML removal)

Sprite / RectangleShape rendering via fixed-function GL quads.
---------------------------------------------------------------------*/
#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "n_draw.h"
#include "n_texture.h"
#include "n_glutil.h"

#include <cmath>

const RenderStates RenderStates::Default{};

using etr_gl::OrthoGuard;

// ---------------------------------------------------------------------------
// Sprite
// ---------------------------------------------------------------------------

Sprite::Sprite()
	: texture_(nullptr), position_(0, 0), scale_(1, 1), color_(255, 255, 255, 255), textureRect_(), useRect_(false) {}

Sprite::Sprite(const Texture& texture) : Sprite() { setTexture(texture, true); }

void Sprite::setTexture(const Texture& texture, bool resetRect) {
	texture_ = &texture;
	if (resetRect) useRect_ = false;
}

void Sprite::setPosition(float x, float y) { position_ = Vector2f(x, y); }
void Sprite::setPosition(const Vector2f& p) { position_ = p; }
void Sprite::setScale(float x, float y) { scale_ = Vector2f(x, y); }
void Sprite::setColor(const Color& c) { color_ = c; }
void Sprite::setTextureRect(const IntRect& r) { textureRect_ = r; useRect_ = true; }

IntRect Sprite::getTextureRect() const noexcept {
	if (texture_ && !useRect_) {
		Vector2u s = texture_->getSize();
		return IntRect(0, 0, static_cast<int>(s.x), static_cast<int>(s.y));
	}
	return textureRect_;
}

FloatRect Sprite::getLocalBounds() const noexcept {
	IntRect r = getTextureRect();
	return FloatRect(0, 0, r.width * scale_.x, r.height * scale_.y);
}

FloatRect Sprite::getGlobalBounds() const noexcept {
	FloatRect lb = getLocalBounds();
	return FloatRect(position_.x + lb.left, position_.y + lb.top, lb.width, lb.height);
}

void Sprite::draw() const {
	if (!texture_ || !texture_->getNativeHandle()) return;

	Vector2u texSize = texture_->getSize();
	if (texSize.x == 0 || texSize.y == 0) return;

	IntRect r = getTextureRect();
	float drawW = r.width  * scale_.x;
	float drawH = r.height * scale_.y;

	float u0 = static_cast<float>(r.left)                / texSize.x;
	float u1 = static_cast<float>(r.left + r.width)      / texSize.x;
	float v0 = static_cast<float>(r.top)                 / texSize.y;
	float v1 = static_cast<float>(r.top + r.height)      / texSize.y;

	float x0 = position_.x, y0 = position_.y;
	float x1 = x0 + drawW,  y1 = y0 + drawH;

	OrthoGuard og;
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);
	Texture::bind(texture_);
	glColor4ub(color_.r, color_.g, color_.b, color_.a);

	glBegin(GL_QUADS);
	glTexCoord2f(u0, v0); glVertex2f(x0, y0);
	glTexCoord2f(u1, v0); glVertex2f(x1, y0);
	glTexCoord2f(u1, v1); glVertex2f(x1, y1);
	glTexCoord2f(u0, v1); glVertex2f(x0, y1);
	glEnd();
}

// ---------------------------------------------------------------------------
// RectangleShape
// ---------------------------------------------------------------------------

RectangleShape::RectangleShape()
	: size_(0, 0), position_(0, 0), fillColor_(255, 255, 255, 255), outlineColor_(255, 255, 255, 255), outlineThickness_(0) {}

RectangleShape::RectangleShape(const Vector2f& size) : RectangleShape() { size_ = size; }

void RectangleShape::setSize(const Vector2f& s) { size_ = s; }
void RectangleShape::setPosition(float x, float y) { position_ = Vector2f(x, y); }
void RectangleShape::setPosition(const Vector2f& p) { position_ = p; }
void RectangleShape::setFillColor(const Color& c) { fillColor_ = c; }
void RectangleShape::setOutlineColor(const Color& c) { outlineColor_ = c; }
void RectangleShape::setOutlineThickness(float t) { outlineThickness_ = t; }

FloatRect RectangleShape::getLocalBounds() const noexcept {
	float t = std::fabs(outlineThickness_);
	return FloatRect(-t, -t, size_.x + 2 * t, size_.y + 2 * t);
}

FloatRect RectangleShape::getGlobalBounds() const noexcept {
	FloatRect lb = getLocalBounds();
	return FloatRect(position_.x + lb.left, position_.y + lb.top, lb.width, lb.height);
}

void RectangleShape::draw() const {
	float x = position_.x, y = position_.y;
	float w = size_.x, h = size_.y;

	OrthoGuard og;
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_DEPTH_TEST);

	// Fill
	glColor4ub(fillColor_.r, fillColor_.g, fillColor_.b, fillColor_.a);
	if (fillColor_.a != 0) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glBegin(GL_QUADS);
		glVertex2f(x,     y);
		glVertex2f(x + w, y);
		glVertex2f(x + w, y + h);
		glVertex2f(x,     y + h);
		glEnd();
	}

	// Inward outline
	float t = outlineThickness_;
	if (t != 0) {
		glColor4ub(outlineColor_.r, outlineColor_.g, outlineColor_.b, outlineColor_.a);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		float a = std::fabs(t);
		glBegin(GL_QUADS);
		// top
		glVertex2f(x,     y);
		glVertex2f(x + w, y);
		glVertex2f(x + w, y + a);
		glVertex2f(x,     y + a);
		// bottom
		glVertex2f(x,     y + h - a);
		glVertex2f(x + w, y + h - a);
		glVertex2f(x + w, y + h);
		glVertex2f(x,     y + h);
		// left
		glVertex2f(x,     y);
		glVertex2f(x + a, y);
		glVertex2f(x + a, y + h);
		glVertex2f(x,     y + h);
		// right
		glVertex2f(x + w - a, y);
		glVertex2f(x + w,     y);
		glVertex2f(x + w,     y + h);
		glVertex2f(x + w - a, y + h);
		glEnd();
	}
}
