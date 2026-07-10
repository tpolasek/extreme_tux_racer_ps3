/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types (SFML removal)

Typeface + Text implementation. FreeType2 renders glyphs into a shared
RGBA atlas texture; Text lays out glyphs and draws colored quads.
---------------------------------------------------------------------*/
#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "n_font.h"
#include "n_glutil.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <GL/gl.h>

#include <cstring>
#include <map>
#include <vector>

// ---------------------------------------------------------------------------
// FreeType library singleton
// ---------------------------------------------------------------------------
static FT_Library ftLibrary() {
	static FT_Library lib = nullptr;
	if (!lib) {
		if (FT_Init_FreeType(&lib) != 0) lib = nullptr;
	}
	return lib;
}

// Minimal UTF-8 -> codepoint decoder.
static std::vector<uint32_t> decodeUtf8(const std::string& s) {
	std::vector<uint32_t> out;
	out.reserve(s.size());
	for (std::size_t i = 0; i < s.size();) {
		unsigned char c = static_cast<unsigned char>(s[i++]);
		uint32_t cp = 0;
		int extra = 0;
		if      (c < 0x80)                                    { cp = c;          extra = 0; }
		else if ((c & 0xE0) == 0xC0)                          { cp = c & 0x1F;   extra = 1; }
		else if ((c & 0xF0) == 0xE0)                          { cp = c & 0x0F;   extra = 2; }
		else if ((c & 0xF8) == 0xF0)                          { cp = c & 0x07;   extra = 3; }
		else                                                  { cp = c;          extra = 0; } // invalid -> literal
		for (int k = 0; k < extra && i < s.size(); ++k) {
			unsigned char d = static_cast<unsigned char>(s[i++]);
			if ((d & 0xC0) != 0x80) { --i; break; }
			cp = (cp << 6) | (d & 0x3F);
		}
		out.push_back(cp);
	}
	return out;
}

// ---------------------------------------------------------------------------
// Glyph cache + atlas
// ---------------------------------------------------------------------------
struct GlyphKey {
	FT_Face  face;
	uint32_t size;
	uint32_t cp;
	bool operator<(const GlyphKey& o) const {
		if (face != o.face) return face < o.face;
		if (size != o.size) return size < o.size;
		return cp < o.cp;
	}
};

struct CachedGlyph {
	float u0, v0, u1, v1;
	int   bitmapLeft, bitmapTop, width, rows;
	float advance;
};

class GlyphAtlas {
	static constexpr unsigned int ATLAS_W = 1024;
	static constexpr unsigned int ATLAS_H = 1024;

	GLuint tex_;
	unsigned int cursorX_, cursorY_, rowH_;
	std::map<GlyphKey, CachedGlyph> cache_;
public:
	GlyphAtlas() : tex_(0), cursorX_(0), cursorY_(0), rowH_(0) {}

	GLuint texture() const { return tex_; }

	const CachedGlyph* get(FT_Face face, uint32_t size, uint32_t cp) {
		GlyphKey key{face, size, cp};
		auto it = cache_.find(key);
		if (it != cache_.end()) return &it->second;

		if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(size)) != 0) return nullptr;
		FT_UInt gi = FT_Get_Char_Index(face, static_cast<FT_ULong>(cp));
		if (gi == 0 && cp != 0) cp = 0; // glyph not found; keep index fallback below
		if (FT_Load_Glyph(face, gi ? gi : FT_Get_Char_Index(face, cp), FT_LOAD_DEFAULT) != 0) return nullptr;
		if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) return nullptr;

		FT_GlyphSlot slot = face->glyph;
		FT_Bitmap& bmp = slot->bitmap;
		unsigned int gw = bmp.width;
		unsigned int gh = bmp.rows;

		ensureTexture();
		if (!tex_) return nullptr;

		unsigned int px = 0, py = 0;
		if (!place(gw, gh, px, py)) return nullptr;

		// Blit grayscale bitmap into RGBA atlas region (white * coverage).
		if (gw > 0 && gh > 0) {
			std::vector<Uint8> rgba(static_cast<std::size_t>(gw) * gh * 4, 0);
			for (unsigned int y = 0; y < gh; ++y) {
				const unsigned char* src = bmp.buffer + static_cast<std::size_t>(y) * bmp.pitch;
				for (unsigned int x = 0; x < gw; ++x) {
					Uint8 a = src[x];
					Uint8* d = &rgba[(static_cast<std::size_t>(y) * gw + x) * 4];
					d[0] = 255; d[1] = 255; d[2] = 255; d[3] = a;
				}
			}
			glBindTexture(GL_TEXTURE_2D, tex_);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			glTexSubImage2D(GL_TEXTURE_2D, 0, px, py, gw, gh, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
		}

		CachedGlyph g;
		g.u0 = static_cast<float>(px)             / ATLAS_W;
		g.v0 = static_cast<float>(py)             / ATLAS_H;
		g.u1 = static_cast<float>(px + gw)        / ATLAS_W;
		g.v1 = static_cast<float>(py + gh)        / ATLAS_H;
		g.bitmapLeft = slot->bitmap_left;
		g.bitmapTop  = slot->bitmap_top;
		g.width      = static_cast<int>(gw);
		g.rows       = static_cast<int>(gh);
		g.advance    = static_cast<float>(slot->advance.x >> 6);
		auto ins = cache_.emplace(key, g);
		return &ins.first->second;
	}

private:
	void ensureTexture() {
		if (tex_) return;
		glGenTextures(1, &tex_);
		if (!tex_) return;
		std::vector<Uint8> zero(static_cast<std::size_t>(ATLAS_W) * ATLAS_H * 4, 0);
		glBindTexture(GL_TEXTURE_2D, tex_);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ATLAS_W, ATLAS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}

	bool place(unsigned int w, unsigned int h, unsigned int& outX, unsigned int& outY) {
		if (cursorX_ + w > ATLAS_W) { cursorX_ = 0; cursorY_ += rowH_; rowH_ = 0; }
		if (cursorY_ + h > ATLAS_H) {
			// Atlas exhausted: reset (re-zero) and start over.
			cursorX_ = cursorY_ = rowH_ = 0;
			std::vector<Uint8> zero(static_cast<std::size_t>(ATLAS_W) * ATLAS_H * 4, 0);
			glBindTexture(GL_TEXTURE_2D, tex_);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ATLAS_W, ATLAS_H, GL_RGBA, GL_UNSIGNED_BYTE, zero.data());
		}
		outX = cursorX_;
		outY = cursorY_;
		cursorX_ += w;
		if (h > rowH_) rowH_ = h;
		return true;
	}
};

static GlyphAtlas& glyphAtlas() {
	static GlyphAtlas a;
	return a;
}

// ---------------------------------------------------------------------------
// Typeface
// ---------------------------------------------------------------------------
Typeface::Typeface() : face_(nullptr), loaded_(false) {}

Typeface::~Typeface() {
	if (face_) FT_Done_Face(face_);
}

bool Typeface::loadFromFile(const std::string& filename) {
	if (face_) { FT_Done_Face(face_); face_ = nullptr; }
	FT_Library lib = ftLibrary();
	if (!lib) return false;
	if (FT_New_Face(lib, filename.c_str(), 0, &face_) != 0) {
		face_ = nullptr;
		loaded_ = false;
		return false;
	}
	loaded_ = true;
	return true;
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
Text::Text()
	: font_(nullptr), charSize_(30), fillColor_(255, 255, 255, 255),
	  outlineColor_(255, 255, 255, 255), outlineThickness_(0), position_(0, 0), boundsDirty_(true) {}

Text::Text(const std::string& string, const Typeface& font, unsigned int characterSize)
	: string_(string), font_(&font), charSize_(characterSize), fillColor_(255, 255, 255, 255),
	  outlineColor_(255, 255, 255, 255), outlineThickness_(0), position_(0, 0), boundsDirty_(true) {}

void Text::setString(const std::string& s)      { string_ = s; boundsDirty_ = true; }
void Text::setFont(const Typeface& font)        { font_ = &font; boundsDirty_ = true; }
void Text::setCharacterSize(unsigned int size)  { charSize_ = size; boundsDirty_ = true; }
void Text::setFillColor(const Color& color)     { fillColor_ = color; }
void Text::setOutlineColor(const Color& color)  { outlineColor_ = color; }
void Text::setOutlineThickness(float t)         { outlineThickness_ = t; }
void Text::setPosition(float x, float y)        { position_ = Vector2f(x, y); }
void Text::setPosition(const Vector2f& p)       { position_ = p; }

void Text::recomputeBounds() const {
	boundsDirty_ = false;
	localBounds_ = FloatRect(0, 0, 0, 0);
	if (!font_ || !font_->isLoaded()) return;

	FT_Face face = font_->getHandle();
	if (FT_Set_Pixel_Sizes(face, 0, charSize_) != 0) return;
	int ascender  = static_cast<int>(face->size->metrics.ascender >> 6);

	float minX = 0, minY = 0, maxX = 0, maxY = 0;
	bool any = false;
	float penX = 0;
	for (uint32_t cp : decodeUtf8(string_)) {
		const CachedGlyph* g = glyphAtlas().get(face, charSize_, cp);
		if (!g) continue;
		any = true;
		float gx = penX + g->bitmapLeft;
		float gy = static_cast<float>(ascender - g->bitmapTop);
		float gr = gx + g->width;
		float gb = gy + g->rows;
		if (!any || gx < minX) minX = gx;
		if (!any || gy < minY) minY = gy;
		if (gr > maxX) maxX = gr;
		if (gb > maxY) maxY = gb;
		penX += g->advance;
	}
	if (any) {
		localBounds_ = FloatRect(minX, minY, maxX - minX, maxY - minY);
	} else {
		localBounds_ = FloatRect(0, 0, penX, 0);
	}
}

FloatRect Text::getLocalBounds() const {
	if (boundsDirty_) recomputeBounds();
	return localBounds_;
}

FloatRect Text::getGlobalBounds() const {
	if (boundsDirty_) recomputeBounds();
	return FloatRect(position_.x + localBounds_.left, position_.y + localBounds_.top,
	                 localBounds_.width, localBounds_.height);
}

void Text::draw() const {
	if (!font_ || !font_->isLoaded()) return;
	FT_Face face = font_->getHandle();
	if (FT_Set_Pixel_Sizes(face, 0, charSize_) != 0) return;
	int ascender = static_cast<int>(face->size->metrics.ascender >> 6);

	GLuint atlas = glyphAtlas().texture();
	if (!atlas) return;

	etr_gl::OrthoGuard og;
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);
	glBindTexture(GL_TEXTURE_2D, atlas);
	glColor4ub(fillColor_.r, fillColor_.g, fillColor_.b, fillColor_.a);

	float penX = position_.x;
	for (uint32_t cp : decodeUtf8(string_)) {
		const CachedGlyph* g = glyphAtlas().get(face, charSize_, cp);
		if (!g) continue;
		float x = penX + g->bitmapLeft;
		float y = position_.y + static_cast<float>(ascender - g->bitmapTop);
		if (g->width > 0 && g->rows > 0) {
			glBegin(GL_QUADS);
			glTexCoord2f(g->u0, g->v0); glVertex2f(x,            y);
			glTexCoord2f(g->u1, g->v0); glVertex2f(x + g->width, y);
			glTexCoord2f(g->u1, g->v1); glVertex2f(x + g->width, y + g->rows);
			glTexCoord2f(g->u0, g->v1); glVertex2f(x,            y + g->rows);
			glEnd();
		}
		penX += g->advance;
	}
}
