/* --------------------------------------------------------------------
EXTREME TUXRACER - native font system

Linux backend: FreeType2 + CPU RGBA RenderSurface + GL texture.
PS3 backend (future): replaces the FontRenderer impl with the
`font.h` API; RenderSurface/Text stay identical in shape.

Texture upload path on Linux:
    FT_Render_Glyph → RenderSurface (RGBA8) → glTexImage2D → quad.

Same logical path on PS3:
    fontRenderCharGlyphImage → fontRenderSurface → RSX texture → quad.
---------------------------------------------------------------------*/
#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include "n_font.h"
#include "n_glutil.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <GL/gl.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

// ---------------------------------------------------------------------------
// FreeType library singleton (one per process).
// ---------------------------------------------------------------------------
static FT_Library ftLibrary() {
	static FT_Library lib = nullptr;
	if (!lib) {
		if (FT_Init_FreeType(&lib) != 0) lib = nullptr;
	}
	return lib;
}

// Minimal UTF-8 → codepoint decoder.
static std::vector<uint32_t> decodeUtf8(const std::string& s) {
	std::vector<uint32_t> out;
	out.reserve(s.size());
	for (std::size_t i = 0; i < s.size();) {
		unsigned char c = static_cast<unsigned char>(s[i++]);
		uint32_t cp = 0;
		int extra = 0;
		if      (c < 0x80)              { cp = c;        extra = 0; }
		else if ((c & 0xE0) == 0xC0)    { cp = c & 0x1F; extra = 1; }
		else if ((c & 0xF0) == 0xE0)    { cp = c & 0x0F; extra = 2; }
		else if ((c & 0xF8) == 0xF0)    { cp = c & 0x07; extra = 3; }
		else                           { cp = c;        extra = 0; } // invalid → literal
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
// Typeface
// ---------------------------------------------------------------------------
Typeface::Typeface() : face_(nullptr), loaded_(false) {}

Typeface::~Typeface() {
	if (face_) FT_Done_Face(face_);
}

bool Typeface::loadFromFile(const std::string& filename) {
	if (face_) { FT_Done_Face(face_); face_ = nullptr; }
	FT_Library lib = ftLibrary();
	if (!lib) { loaded_ = false; return false; }
	if (FT_New_Face(lib, filename.c_str(), 0, &face_) != 0) {
		face_ = nullptr;
		loaded_ = false;
		return false;
	}
	loaded_ = true;
	return true;
}

// ---------------------------------------------------------------------------
// RenderSurface
// ---------------------------------------------------------------------------
RenderSurface::RenderSurface() : pixels_(nullptr), width_(0), height_(0), stride_(0) {}

RenderSurface::RenderSurface(unsigned int w, unsigned int h)
	: pixels_(nullptr), width_(0), height_(0), stride_(0) {
	allocate(w, h);
}

RenderSurface::~RenderSurface() { release(); }

void RenderSurface::allocate(unsigned int w, unsigned int h) {
	release();
	if (w == 0 || h == 0) return;
	width_  = w;
	height_ = h;
	stride_ = w * 4;
	pixels_ = static_cast<Uint8*>(std::malloc(static_cast<std::size_t>(stride_) * h));
	if (pixels_) std::memset(pixels_, 0, static_cast<std::size_t>(stride_) * h);
}

void RenderSurface::release() {
	if (pixels_) std::free(pixels_);
	pixels_ = nullptr;
	width_ = height_ = stride_ = 0;
}

void RenderSurface::clear() {
	if (pixels_) std::memset(pixels_, 0, static_cast<std::size_t>(stride_) * height_);
}

void RenderSurface::blendPixel(unsigned int x, unsigned int y, const Color& color, Uint8 coverage) {
	if (!pixels_) return;
	if (x >= width_ || y >= height_) return;
	// Source-over alpha blend: dst = dst*(1-a) + src*a, where a = coverage*color.a/255^2.
	unsigned ca = (static_cast<unsigned>(coverage) * color.a + 128) / 255;
	if (ca == 0) return;
	Uint8* p = pixels_ + (static_cast<std::size_t>(y) * stride_ + static_cast<std::size_t>(x) * 4);
	unsigned inv = 255 - ca;
	p[0] = static_cast<Uint8>((static_cast<unsigned>(p[0]) * inv + static_cast<unsigned>(color.r) * ca) / 255);
	p[1] = static_cast<Uint8>((static_cast<unsigned>(p[1]) * inv + static_cast<unsigned>(color.g) * ca) / 255);
	p[2] = static_cast<Uint8>((static_cast<unsigned>(p[2]) * inv + static_cast<unsigned>(color.b) * ca) / 255);
	p[3] = static_cast<Uint8>(ca + (static_cast<unsigned>(p[3]) * inv) / 255);
}

// ---------------------------------------------------------------------------
// FontRenderer: glyph cache + rasterizer.
// ---------------------------------------------------------------------------
struct FontRenderer::Impl {
	FT_Face  face       = nullptr;
	unsigned pixelSize  = 0;
	float    ascender   = 0;
	float    descender  = 0;
	float    lineHeight = 0;

	struct Key {
		FT_Face  face;
		unsigned size;
		uint32_t cp;
		bool operator<(const Key& o) const {
			if (face != o.face) return face < o.face;
			if (size != o.size) return size < o.size;
			return cp < o.cp;
		}
	};

	struct Cached {
		GlyphMetrics            metrics;
		std::vector<Uint8>      rgba;     // RGBA8 bitmap, width*height*4
	};

	std::map<Key, Cached> cache;

	bool ensureSize(unsigned size) {
		if (!face) return false;
		if (pixelSize == size) return true;
		if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(size)) != 0) return false;
		pixelSize = size;
		ascender   = static_cast<float>(face->size->metrics.ascender  >> 6);
		descender  = -static_cast<float>(face->size->metrics.descender >> 6); // FT descender < 0
		lineHeight = static_cast<float>(face->size->metrics.height     >> 6);
		return true;
	}

	const Cached* rasterize(uint32_t cp) {
		if (!face) return nullptr;
		Key key{face, pixelSize, cp};
		auto it = cache.find(key);
		if (it != cache.end()) return &it->second;

		FT_UInt gi = FT_Get_Char_Index(face, static_cast<FT_ULong>(cp));
		if (gi == 0) return nullptr; // no glyph for this codepoint
		if (FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT) != 0) return nullptr;
		if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) return nullptr;

		FT_GlyphSlot slot = face->glyph;
		FT_Bitmap& bmp = slot->bitmap;
		unsigned int gw = bmp.width;
		unsigned int gh = bmp.rows;

		Cached c;
		c.metrics.advanceX = static_cast<float>(slot->advance.x >> 6);
		c.metrics.bearingX = static_cast<float>(slot->bitmap_left);
		c.metrics.bearingY = static_cast<float>(slot->bitmap_top);
		c.metrics.width    = gw;
		c.metrics.height   = gh;
		c.rgba.assign(static_cast<std::size_t>(gw) * gh * 4, 0);

		// Convert FreeType grayscale coverage bitmap → RGBA8 (white * coverage).
		// Color modulation happens at blend time on the surface.
		for (unsigned int y = 0; y < gh; ++y) {
			const unsigned char* src = bmp.buffer + static_cast<std::size_t>(y) * bmp.pitch;
			for (unsigned int x = 0; x < gw; ++x) {
				Uint8 a = src[x];
				Uint8* d = &c.rgba[(static_cast<std::size_t>(y) * gw + x) * 4];
				d[0] = 255; d[1] = 255; d[2] = 255; d[3] = a;
			}
		}

		auto ins = cache.emplace(key, std::move(c));
		return &ins.first->second;
	}
};

FontRenderer::FontRenderer() : impl_(new Impl) {}
FontRenderer::~FontRenderer() = default; // unique_ptr<Impl> needs complete type here.

void FontRenderer::setTypeface(const Typeface& face) {
	impl_->face = face.isLoaded() ? face.getHandle() : nullptr;
	impl_->pixelSize = 0; // force re-application of scale on next use
}

void FontRenderer::setScale(float pixelWidth, float pixelHeight) {
	(void)pixelWidth; // FreeType selects width from height; PS3 path uses both.
	impl_->ensureSize(static_cast<unsigned>(pixelHeight > 0 ? pixelHeight : 16));
}

GlyphMetrics FontRenderer::getMetrics(uint32_t codepoint) {
	const Impl::Cached* c = impl_->rasterize(codepoint);
	if (c) return c->metrics;
	GlyphMetrics zero{0, 0, 0, 0, 0};
	return zero;
}

GlyphMetrics FontRenderer::renderChar(uint32_t codepoint, RenderSurface& surface,
                                       float penX, float penY, const Color& fillColor,
                                       float outlineThickness, const Color& outlineColor) {
	const Impl::Cached* c = impl_->rasterize(codepoint);
	if (!c) return GlyphMetrics{0, 0, 0, 0, 0};

	// penY is the baseline; bearingY is the offset up from baseline to bitmap top.
	int baseX = static_cast<int>(penX + c->metrics.bearingX + 0.5f);
	int baseY = static_cast<int>(penY - c->metrics.bearingY + 0.5f);

	// Local blit helper: stamp glyph bitmap at (originX, originY) modulated by `col`.
	auto blitGlyph = [&](int originX, int originY, const Color& col) {
		for (unsigned int y = 0; y < c->metrics.height; ++y) {
			for (unsigned int x = 0; x < c->metrics.width; ++x) {
				Uint8 coverage = c->rgba[(static_cast<std::size_t>(y) * c->metrics.width + x) * 4 + 3];
				if (coverage == 0) continue;
				surface.blendPixel(static_cast<unsigned int>(originX) + x,
				                   static_cast<unsigned int>(originY) + y,
				                   col, coverage);
			}
		}
	};

	// Outline pass: stamp the glyph at 8-direction offsets (±outlineThickness)
	// in outlineColor first. Source-over blend (RenderSurface::blendPixel)
	// means the later fill pass overwrites the outline inside the glyph body
	// while leaving the outline halo visible around it.
	int t = static_cast<int>(outlineThickness + 0.5f);
	if (t > 0) {
		for (int dy = -t; dy <= t; ++dy) {
			for (int dx = -t; dx <= t; ++dx) {
				if (dx == 0 && dy == 0) continue;
				blitGlyph(baseX + dx, baseY + dy, outlineColor);
			}
		}
	}

	blitGlyph(baseX, baseY, fillColor);
	return c->metrics;
}

float FontRenderer::getAscender()  const noexcept { return impl_->ascender; }
float FontRenderer::getDescender() const noexcept { return impl_->descender; }
float FontRenderer::getLineHeight() const noexcept { return impl_->lineHeight; }

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
Text::Text()
	: face_(nullptr), charSize_(30),
	  fillColor_(255, 255, 255, 255), outlineColor_(255, 255, 255, 255),
	  outlineThickness_(0), position_(0, 0),
	  boundsDirty_(true), localBounds_(0, 0, 0, 0), baselineY_(0),
	  textureDirty_(true), texture_(0), texW_(0), texH_(0) {}

Text::Text(const std::string& string, const Typeface& font, unsigned int characterSize)
	: string_(string), face_(&font), charSize_(characterSize),
	  fillColor_(255, 255, 255, 255), outlineColor_(255, 255, 255, 255),
	  outlineThickness_(0), position_(0, 0),
	  boundsDirty_(true), localBounds_(0, 0, 0, 0), baselineY_(0),
	  textureDirty_(true), texture_(0), texW_(0), texH_(0) {}

Text::~Text() {
	if (texture_) glDeleteTextures(1, &texture_);
}

void Text::setString(const std::string& s) {
	if (s != string_) { string_ = s; boundsDirty_ = true; textureDirty_ = true; }
}
void Text::setFont(const Typeface& font) {
	if (&font != face_) { face_ = &font; boundsDirty_ = true; textureDirty_ = true; }
}
void Text::setCharacterSize(unsigned int size) {
	if (size != charSize_) { charSize_ = size; boundsDirty_ = true; textureDirty_ = true; }
}
void Text::setFillColor(const Color& c) {
	if (c.r != fillColor_.r || c.g != fillColor_.g || c.b != fillColor_.b || c.a != fillColor_.a) {
		fillColor_ = c; textureDirty_ = true;
	}
}
void Text::setOutlineColor(const Color& c) {
	if (c.r != outlineColor_.r || c.g != outlineColor_.g ||
	    c.b != outlineColor_.b || c.a != outlineColor_.a) {
		outlineColor_ = c;
		textureDirty_ = true;
	}
}
void Text::setOutlineThickness(float t) {
	if (t != outlineThickness_) {
		outlineThickness_ = t;
		boundsDirty_ = true;   // outline grows the ink box
		textureDirty_ = true;
	}
}
void Text::setPosition(float x, float y)    { position_ = Vector2f(x, y); }
void Text::setPosition(const Vector2f& p)   { position_ = p; }

void Text::recomputeBounds() const {
	boundsDirty_ = false;
	localBounds_ = FloatRect(0, 0, 0, 0);
	baselineY_ = 0;
	if (!face_ || !face_->isLoaded()) return;

	FontRenderer fr;
	fr.setTypeface(*face_);
	fr.setScale(0, static_cast<float>(charSize_));

	float ascender  = fr.getAscender();
	float descender = fr.getDescender(); // negative
	float penX = 0;
	float minY = 0, maxY = 0;
	bool any = false;

	for (uint32_t cp : decodeUtf8(string_)) {
		GlyphMetrics m = fr.getMetrics(cp);
		if (m.width == 0 && m.height == 0 && m.advanceX == 0) {
			penX += 0;
			continue;
		}
		any = true;
		float top = ascender - m.bearingY;
		float bot = top + static_cast<float>(m.height);
		if (top < minY) minY = top;
		if (bot > maxY) maxY = bot;
		penX += m.advanceX;
	}

	if (!any) {
		localBounds_ = FloatRect(0, 0, 0, 0);
		return;
	}
	// Round texture size up to whole pixels.
	int w = static_cast<int>(penX + 0.5f);
	if (w < 1) w = 1;
	int h = static_cast<int>(maxY - minY + 0.5f);
	if (h < 1) h = 1;

	// Outline extends ±outlineThickness_ pixels around every glyph. Grow the
	// ink box by 2t on each axis and shift the baseline down by t so the
	// topmost glyph still has t pixels of headroom for its outline.
	int pad = static_cast<int>(outlineThickness_ + 0.5f);
	if (pad > 0) {
		w += 2 * pad;
		h += 2 * pad;
	}

	localBounds_ = FloatRect(0, 0, static_cast<float>(w), static_cast<float>(h));
	baselineY_ = ascender - minY + static_cast<float>(pad); // baseline offset from top of texture
}

FloatRect Text::getLocalBounds() const {
	if (boundsDirty_) recomputeBounds();
	return localBounds_;
}

FloatRect Text::getGlobalBounds() const {
	if (boundsDirty_) recomputeBounds();
	return FloatRect(position_.x + localBounds_.left,
	                 position_.y + localBounds_.top,
	                 localBounds_.width, localBounds_.height);
}

void Text::updateTexture() const {
	textureDirty_ = false;
	if (!face_ || !face_->isLoaded()) return;
	if (localBounds_.width < 1 || localBounds_.height < 1) {
		if (texture_) { glDeleteTextures(1, &texture_); texture_ = 0; }
		texW_ = texH_ = 0;
		return;
	}

	texW_ = static_cast<unsigned int>(localBounds_.width  + 0.5f);
	texH_ = static_cast<unsigned int>(localBounds_.height + 0.5f);
	if (texW_ == 0 || texH_ == 0) return;

	// Render string into a CPU RGBA surface via FontRenderer.
	RenderSurface surface(texW_, texH_);
	FontRenderer fr;
	fr.setTypeface(*face_);
	fr.setScale(0, static_cast<float>(charSize_));

	// Pen starts at (pad, baselineY_) so the outline halo has `pad` pixels
	// of room on the left and top edges of the surface (see recomputeBounds).
	int pad = static_cast<int>(outlineThickness_ + 0.5f);
	float penX = static_cast<float>(pad);
	float penY = baselineY_;
	for (uint32_t cp : decodeUtf8(string_)) {
		GlyphMetrics m = fr.renderChar(cp, surface, penX, penY, fillColor_,
		                                outlineThickness_, outlineColor_);
		penX += m.advanceX;
	}

	// Upload as a GL texture (PS3 path is identical: CPU buffer → RSX texture).
	if (!texture_) glGenTextures(1, &texture_);
	glBindTexture(GL_TEXTURE_2D, texture_);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texW_, texH_, 0, GL_RGBA, GL_UNSIGNED_BYTE, surface.pixels());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void Text::draw() const {
	if (boundsDirty_)  recomputeBounds();
	if (textureDirty_) updateTexture();
	if (!texture_ || texW_ == 0 || texH_ == 0) return;

	etr_gl::OrthoGuard og;
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);
	glBindTexture(GL_TEXTURE_2D, texture_);
	glColor4ub(255, 255, 255, 255);

	float x = position_.x;
	float y = position_.y;
	float w = static_cast<float>(texW_);
	float h = static_cast<float>(texH_);

	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2f(x,     y);
	glTexCoord2f(1, 0); glVertex2f(x + w, y);
	glTexCoord2f(1, 1); glVertex2f(x + w, y + h);
	glTexCoord2f(0, 1); glVertex2f(x,     y + h);
	glEnd();
}
