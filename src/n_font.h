/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types (SFML removal)

Typeface (replaces sf::Font; renamed to dodge X11's global `Font` typedef)
+ Text. FreeType2 is used internally; the header only forward-declares
FT_Face so <ft2build.h> does not propagate to the rest of the codebase.
---------------------------------------------------------------------*/
#ifndef N_FONT_H
#define N_FONT_H

#include "n_color.h"
#include "n_geom.h"

#include <string>

// Opaque FreeType face handle (defined identically in <ft2build.h>).
typedef struct FT_FaceRec_* FT_Face;

class Typeface {
	FT_Face face_;
	bool loaded_;
public:
	Typeface();
	~Typeface();
	Typeface(const Typeface&) = delete;
	Typeface& operator=(const Typeface&) = delete;

	bool loadFromFile(const std::string& filename);
	bool isLoaded() const noexcept { return loaded_; }
	FT_Face getHandle() const noexcept { return face_; }
};

class Text {
	std::string string_;
	const Typeface* font_;
	unsigned int charSize_;
	Color fillColor_;
	Color outlineColor_;
	float outlineThickness_;
	Vector2f position_;
	mutable bool boundsDirty_;
	mutable FloatRect localBounds_;
public:
	Text();
	Text(const std::string& string, const Typeface& font, unsigned int characterSize = 30);

	void setString(const std::string& s);
	void setFont(const Typeface& font);
	void setCharacterSize(unsigned int size);
	void setFillColor(const Color& color);
	void setOutlineColor(const Color& color);
	void setOutlineThickness(float thickness);
	void setPosition(float x, float y);
	void setPosition(const Vector2f& p);

	FloatRect getLocalBounds() const;
	FloatRect getGlobalBounds() const;

	void draw() const;
private:
	void recomputeBounds() const;
};

#endif // N_FONT_H
