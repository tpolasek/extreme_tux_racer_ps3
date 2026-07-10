/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types

2D drawing primitives: Drawable2D (named to avoid X11's global
`Drawable` typedef), RenderStates, Sprite, RectangleShape.

Each Drawable renders into a self-contained y-down ortho projection
queried from the current GL viewport, so it is independent of the
caller's matrix state.
---------------------------------------------------------------------*/
#ifndef N_DRAW_H
#define N_DRAW_H

#include "n_color.h"
#include "n_geom.h"

#include <GL/gl.h>

class Texture;

// Named to avoid X11's global `Drawable` typedef.
class Drawable2D {
public:
	virtual ~Drawable2D() {}
	virtual void draw() const = 0;
};

struct RenderStates {
	RenderStates() = default;
	static const RenderStates Default;
};

class Sprite : public Drawable2D {
	const Texture* texture_;
	Vector2f position_;
	Vector2f scale_;
	Color color_;
	IntRect textureRect_;
	bool useRect_;
public:
	Sprite();
	explicit Sprite(const Texture& texture);

	void setTexture(const Texture& texture, bool resetRect = true);
	void setPosition(float x, float y);
	void setPosition(const Vector2f& p);
	void setScale(float x, float y);
	void setColor(const Color& c);
	void setTextureRect(const IntRect& r);

	const Texture* getTexture() const noexcept { return texture_; }
	IntRect getTextureRect() const noexcept;
	Vector2f getPosition() const noexcept { return position_; }
	Vector2f getScale() const noexcept { return scale_; }
	FloatRect getLocalBounds() const noexcept;
	FloatRect getGlobalBounds() const noexcept;

	void draw() const override;
};

class RectangleShape : public Drawable2D {
	Vector2f size_;
	Vector2f position_;
	Color fillColor_;
	Color outlineColor_;
	float outlineThickness_;
public:
	RectangleShape();
	explicit RectangleShape(const Vector2f& size);

	void setSize(const Vector2f& s);
	void setPosition(float x, float y);
	void setPosition(const Vector2f& p);
	void setFillColor(const Color& c);
	void setOutlineColor(const Color& c);
	void setOutlineThickness(float t);

	Vector2f getSize() const noexcept { return size_; }
	Vector2f getPosition() const noexcept { return position_; }
	FloatRect getLocalBounds() const noexcept;
	FloatRect getGlobalBounds() const noexcept;

	void draw() const override;
};

#endif // N_DRAW_H
