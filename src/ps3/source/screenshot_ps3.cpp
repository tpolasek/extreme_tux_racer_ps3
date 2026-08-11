/* --------------------------------------------------------------------
EXTREME TUXRACER - DEMO_MODE framebuffer screenshot (PS3)

Reads the displayed color surface back from RSX memory and writes a PNG
via the game's Image class. Optionally downscales to fit a target box,
preserving display aspect with bilinear filtering.

Only linked into the PS3 build; only called under #ifdef DEMO_MODE.
---------------------------------------------------------------------*/
#include "screenshot.h"

#include "rsxutil.h"
#include "n_image.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

/* Repack the X8R8G8B8 color surface into an RGBA8 Image.
 * On PS3 (big-endian PPU) a pixel word read from color_buffer is 0xXXRRGGBB. */
static Image captureFrontRGBA() {
	rsxWaitIdle();
	__asm__ __volatile__("sync" ::: "memory");

	const u32 front = curr_fb ^ 1;             // just-displayed surface (post-flip)
	const u32 *src = color_buffer[front];
	const unsigned w = display_width;
	const unsigned h = display_height;
	const unsigned rowWords = color_pitch / 4; // u32 stride (padded to 64B)

	std::vector<Uint8> rgba(static_cast<std::size_t>(w) * h * 4);
	for (unsigned y = 0; y < h; y++) {
		const u32 *row = src + y * rowWords;
		Uint8 *out = rgba.data() + static_cast<std::size_t>(y) * w * 4;
		for (unsigned x = 0; x < w; x++) {
			const u32 v = row[x];
			out[x * 4 + 0] = (v >> 16) & 0xFF;   // R
			out[x * 4 + 1] = (v >> 8)  & 0xFF;   // G
			out[x * 4 + 2] =  v        & 0xFF;   // B
			out[x * 4 + 3] = 0xFF;               // A (framebuffer is X8R8G8B8)
		}
	}

	Image img;
	img.create(w, h, rgba.data());
	return img;
}

/* Compute output dimensions that preserve `aspect` while fitting in (maxW,maxH). */
static void fitBox(unsigned srcW, unsigned srcH, float aspect,
                   int maxW, int maxH, unsigned &outW, unsigned &outH) {
	const float boxAspect = float(maxW) / float(maxH);
	unsigned w, h;
	if (aspect >= boxAspect) {            // width-bound
		w = maxW;
		h = unsigned(float(maxW) / aspect + 0.5f);
	} else {                             // height-bound
		h = maxH;
		w = unsigned(float(maxH) * aspect + 0.5f);
	}
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	if (w > srcW) w = srcW;              // never upsample
	if (h > srcH) h = srcH;
	outW = w;
	outH = h;
}

static Color lerpColor(const Color &a, const Color &b, float t) {
	return Color(
	    Uint8(a.r + (b.r - a.r) * t),
	    Uint8(a.g + (b.g - a.g) * t),
	    Uint8(a.b + (b.b - a.b) * t),
	    Uint8(a.a + (b.a - a.a) * t));
}

/* Bilinear resample preserving display aspect (corrects anamorphic SD). */
static Image bilinearResize(const Image &src, unsigned outW, unsigned outH) {
	const unsigned sw = src.getWidth();
	const unsigned sh = src.getHeight();
	Image dst;
	dst.create(outW, outH, Color(0, 0, 0, 255));
	for (unsigned ty = 0; ty < outH; ty++) {
		float sy = (float(ty) + 0.5f) * sh / outH - 0.5f;
		if (sy < 0.f) sy = 0.f;
		if (sy > sh - 1.f) sy = float(sh - 1);
		const unsigned y0 = unsigned(sy);
		const unsigned y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
		const float fy = sy - float(y0);
		for (unsigned tx = 0; tx < outW; tx++) {
			float sx = (float(tx) + 0.5f) * sw / outW - 0.5f;
			if (sx < 0.f) sx = 0.f;
			if (sx > sw - 1.f) sx = float(sw - 1);
			const unsigned x0 = unsigned(sx);
			const unsigned x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
			const float fx = sx - float(x0);
			const Color c00 = src.getPixel(x0, y0);
			const Color c10 = src.getPixel(x1, y0);
			const Color c01 = src.getPixel(x0, y1);
			const Color c11 = src.getPixel(x1, y1);
			const Color top = lerpColor(c00, c10, fx);
			const Color bot = lerpColor(c01, c11, fx);
			dst.setPixel(tx, ty, lerpColor(top, bot, fy));
		}
	}
	return dst;
}

void demoScreenshot(const char *path, int maxW, int maxH) {
	Image full = captureFrontRGBA();

	bool downscale = (maxW > 0 && maxH > 0);
	if (downscale) {
		unsigned outW, outH;
		fitBox(display_width, display_height, aspect_ratio, maxW, maxH, outW, outH);
		if (outW != full.getWidth() || outH != full.getHeight()) {
			Image small = bilinearResize(full, outW, outH);
			if (!small.saveToFile(path))
				std::printf("[etr] screenshot save FAILED: %s\n", path);
			return;
		}
	}
	if (!full.saveToFile(path))
		std::printf("[etr] screenshot save FAILED: %s\n", path);
}
