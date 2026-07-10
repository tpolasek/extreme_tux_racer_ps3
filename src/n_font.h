/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types

Font system designed to mirror the PS3 `font.h` API exactly so the
Linux backend (FreeType2 + CPU RGBA surface + GL texture) maps 1:1 to
the PS3 backend (`font` + `fontRenderer` + `fontRenderSurface` +
RSX texture).

Concept correspondence:

  Linux (this file)            PS3 (font.h)
  ----------------------       ----------------------
  Typeface                     font            (loaded face)
  FontRenderer                 fontRenderer    (glyph cache)
  RenderSurface                fontRenderSurface (CPU pixel target)
  GlyphMetrics                 fontGlyphMetrics
  FontRenderer.renderChar      fontRenderCharGlyphImage
  Typeface.loadFromFile        fontOpenFontFile
  FontRenderer.setScale        fontSetScalePixel

High-level Text renders a string to a CPU RGBA buffer via
FontRenderer, uploads it as a GL texture, and draws a textured quad.
The PS3 port follows the identical path: render → CPU buffer → RSX
texture → quad. Only the FontRenderer backend changes.
---------------------------------------------------------------------*/
#ifndef N_FONT_H
#define N_FONT_H

#include "n_color.h"
#include "n_geom.h"
#include "n_draw.h"   // Text inherits Drawable2D so Winsys.draw(text) works.

#include <cstdint>
#include <memory>
#include <string>

// Opaque FreeType face handle (Linux backend). The PS3 backend would
// forward-decl its own handle types (`font`, `fontRenderer`).
typedef struct FT_FaceRec_* FT_Face;

// --------------------------------------------------------------------
// Typeface — a loaded font file.
//   Linux: wraps FT_Face.
//   PS3  : wraps `font` (open via fontOpenFontFile or fontOpenFontset).
// --------------------------------------------------------------------
class Typeface {
	FT_Face face_;
	bool    loaded_;
public:
	Typeface();
	~Typeface();
	Typeface(const Typeface&) = delete;
	Typeface& operator=(const Typeface&) = delete;

	bool loadFromFile(const std::string& filename);
	bool isLoaded() const noexcept { return loaded_; }
	FT_Face getHandle() const noexcept { return face_; } // Linux-only
};

// --------------------------------------------------------------------
// RenderSurface — CPU pixel buffer that glyphs are rasterized into.
// RGBA8, top-left origin. Mirrors PS3's fontRenderSurface.
// --------------------------------------------------------------------
class RenderSurface {
	Uint8*       pixels_;
	unsigned int width_, height_, stride_; // stride in bytes
public:
	RenderSurface();
	explicit RenderSurface(unsigned int width, unsigned int height);
	~RenderSurface();
	RenderSurface(const RenderSurface&) = delete;
	RenderSurface& operator=(const RenderSurface&) = delete;

	void allocate(unsigned int width, unsigned int height);
	void release();
	void clear(); // zero-fill

	// Blend a glyph pixel into (x,y) using `coverage` (0..255) as alpha.
	void blendPixel(unsigned int x, unsigned int y, const Color& color, Uint8 coverage);

	Uint8*       pixels()       noexcept { return pixels_; }
	const Uint8* pixels() const noexcept { return pixels_; }
	unsigned int width()  const noexcept { return width_; }
	unsigned int height() const noexcept { return height_; }
	unsigned int stride() const noexcept { return stride_; }
};

// --------------------------------------------------------------------
// GlyphMetrics — cross-platform metrics returned by FontRenderer.
// Mirrors PS3 fontGlyphMetrics (subset used by the game).
// --------------------------------------------------------------------
struct GlyphMetrics {
	float        advanceX;  // horizontal advance to next char (pixels)
	float        bearingX;  // left bearing from pen origin (pixels)
	float        bearingY;  // top bearing above baseline (positive=up) (pixels)
	unsigned int width;     // glyph bitmap width
	unsigned int height;    // glyph bitmap height
};

// --------------------------------------------------------------------
// FontRenderer — glyph cache + rasterizer bound to a Typeface + scale.
// Mirrors PS3 fontRenderer (which holds the glyph cache, allocated via
// fontCreateRenderer).
//
// Usage: setTypeface() + setScale() once per draw sequence, then call
// renderChar() for each codepoint. Internally caches rasterized glyphs
// by (face, pixel-size, codepoint) so repeat draws are pure blits.
// --------------------------------------------------------------------
class FontRenderer {
public:
	FontRenderer();
	~FontRenderer();
	FontRenderer(const FontRenderer&) = delete;
	FontRenderer& operator=(const FontRenderer&) = delete;

	void setTypeface(const Typeface& face);
	void setScale(float pixelWidth, float pixelHeight); // pixelWidth==0 → derive from height

	// Query metrics without rasterizing.
	GlyphMetrics getMetrics(uint32_t codepoint);

	// Rasterize one glyph onto `surface` at baseline position (penX, penY).
	// The glyph bitmap is alpha-modulated by `color`.
	GlyphMetrics renderChar(uint32_t codepoint, RenderSurface& surface,
	                        float penX, float penY, const Color& color);

	// Vertical metrics at the current scale.
	float getAscender() const noexcept;
	float getDescender() const noexcept;
	float getLineHeight() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// --------------------------------------------------------------------
// Text — high-level string widget.
//
// Renders a UTF-8 string to a CPU RGBA surface via FontRenderer,
// uploads it as a GL texture, and draws a textured quad. The same
// path works on PS3 (CPU buffer → RSX texture → quad); only the
// FontRenderer backend differs.
//
// API mirrors the subset of the old text API used by the game so callers
// (CFont::DrawText, TLabel, TFramedText, ...) don't change shape.
// --------------------------------------------------------------------
class Text : public Drawable2D {
	std::string       string_;
	const Typeface*   face_;
	unsigned int      charSize_;
	Color             fillColor_;
	Color             outlineColor_;   // kept for API compat (no separate outline pass)
	float             outlineThickness_; // kept for API compat (ignored)
	Vector2f          position_;

	mutable bool      boundsDirty_;
	mutable FloatRect localBounds_;
	mutable float     baselineY_;       // distance from top of texture to baseline

	mutable bool      textureDirty_;
	mutable GLuint    texture_;
	mutable unsigned int texW_, texH_;

	void recomputeBounds() const;
	void updateTexture() const;
public:
	Text();
	Text(const std::string& string, const Typeface& font, unsigned int characterSize = 30);
	~Text();
	Text(const Text&) = delete;
	Text& operator=(const Text&) = delete;

	void setString(const std::string& s);
	void setFont(const Typeface& font);
	void setCharacterSize(unsigned int size);
	void setFillColor(const Color& c);
	void setOutlineColor(const Color& c);
	void setOutlineThickness(float t);
	void setPosition(float x, float y);
	void setPosition(const Vector2f& p);

	FloatRect getLocalBounds() const;
	FloatRect getGlobalBounds() const;

	void draw() const override;
};

#endif // N_FONT_H
