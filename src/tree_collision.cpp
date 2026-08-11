/* --------------------------------------------------------------------
EXTREME TUXRACER — tree silhouette collision (implementation)

Build: PNG -> Image (RGBA8, top-left origin) -> box-decimate to W x H ->
threshold alpha >= 128 -> bit-pack into rows[].

Test: project Tux's sphere onto the tree's billboard plane, compute the
disc radius on that plane (sphere ∩ plane), pad the projected (u,v) by
the disc radius in UV, scan the overlapping texel bbox, early-exit on
the first opaque bit.

The renderer's texCoords map PNG row 0 to the TOP of the tree, so mask
row 0 (top of `rows[]`) corresponds to world V = 1 (top of tree). The
lookup inverts V.
---------------------------------------------------------------------*/

#include "tree_collision.h"

#include "mathlib.h"      // TVector3d
#include "n_image.h"      // Image
#include "spx.h"          // MakePathStr
#include "game_config.h"  // param

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

// Mask resolution. ~1 KB per type; one texel is 1-7 cm on typical 1-9 m
// trees, well below Tux's 0.6 m radius.
constexpr int kMaskW = 64;
constexpr int kMaskH = 128;
// Match ogl.cpp:279 glAlphaFunc(GL_GEQUAL, 0.5): any texel that draws
// should also block.
constexpr int kAlphaThreshold = 128;

} // namespace

// --------------------------------------------------------------------
//                       silhouette build
// --------------------------------------------------------------------

void BuildTreeSilhouette(TObjectType& type) {
	TreeSilhouette& sil = type.silhouette;
	sil.rows.clear();
	sil.W = 0;
	sil.H = 0;
	sil.maxHalfWidthFrac = 0.f;
	sil.minYFrac = 0.f;
	sil.maxYFrac = 0.f;

	if (type.textureFile.empty()) return;

	Image img;
	const std::string path = MakePathStr(param.obj_dir, type.textureFile);
	if (!img.loadFromFile(path)) return;
	// PNG top-left == GL texture row 0 == top of rendered tree. Do NOT
	// flip — flipping here would invert collision vertically.

	const unsigned int srcW = img.getWidth();
	const unsigned int srcH = img.getHeight();
	if (srcW == 0 || srcH == 0) return;

	const Uint8* px = img.getPixelsPtr();

	sil.W = static_cast<uint16_t>(kMaskW);
	sil.H = static_cast<uint16_t>(kMaskH);
	sil.rows.assign((std::size_t)kMaskH * ((kMaskW + 31) / 32), 0);

	const double sx = static_cast<double>(srcW) / kMaskW;
	const double sy = static_cast<double>(srcH) / kMaskH;
	const int isx = static_cast<int>(srcW);
	const int isy = static_cast<int>(srcH);

	int firstOpaqueRow = -1;
	int lastOpaqueRow = -1;
	int maxOpaqueCol = -1;

	const int wordsPerRow = (kMaskW + 31) / 32;

	for (int my = 0; my < kMaskH; ++my) {
		// Inclusive source-row range covering this mask row. srcY1 can
		// fall back to srcY0 when sy < 1 (upsampling); the clamp to
		// srcH-1 keeps it in range.
		const int srcY0 = std::min(static_cast<int>(my * sy), isy - 1);
		const int srcY1 = std::min(static_cast<int>((my + 1) * sy), isy - 1);
		const int yEnd = std::max(srcY0, srcY1);

		const std::size_t rowWordOff = static_cast<std::size_t>(my) * wordsPerRow;
		bool rowHasOpaque = false;

		for (int mx = 0; mx < kMaskW; ++mx) {
			const int srcX0 = std::min(static_cast<int>(mx * sx), isx - 1);
			const int srcX1 = std::min(static_cast<int>((mx + 1) * sx), isx - 1);
			const int xEnd = std::max(srcX0, srcX1);

			bool opaque = false;
			for (int y = srcY0; y <= yEnd && !opaque; ++y) {
				const Uint8* row = px + (std::size_t)y * srcW * 4;
				for (int x = srcX0; x <= xEnd; ++x) {
					if (row[x * 4 + 3] >= kAlphaThreshold) {
						opaque = true;
						break;
					}
				}
			}

			if (opaque) {
				sil.rows[rowWordOff + (mx >> 5)] |= (1u << (31 - (mx & 31)));
				rowHasOpaque = true;
				if (mx > maxOpaqueCol) maxOpaqueCol = mx;
			}
		}

		if (rowHasOpaque) {
			if (firstOpaqueRow < 0) firstOpaqueRow = my;
			lastOpaqueRow = my;
		}
	}

	if (firstOpaqueRow < 0) return;  // empty silhouette, W/H set but rows zero

	sil.maxHalfWidthFrac = static_cast<float>(maxOpaqueCol + 1) / kMaskW;
	// Row 0 == top of tree == V = 1, so invert when converting row → V frac.
	sil.minYFrac = static_cast<float>(kMaskH - lastOpaqueRow) / kMaskH;
	sil.maxYFrac = static_cast<float>(kMaskH - firstOpaqueRow) / kMaskH;

	// Envelope polygon: per opaque row, leftmost + rightmost opaque column.
	// Walked top→bottom on the left, bottom→top on the right to form a
	// closed line loop. Stored in world-UV (v bottom-up).
	auto rowEnds = [&](int my, int& xL, int& xR) {
		xL = -1; xR = -1;
		const std::size_t rowOff = static_cast<std::size_t>(my) * wordsPerRow;
		for (int x = 0; x < kMaskW; ++x) {
			if (sil.rows[rowOff + (x >> 5)] & (1u << (31 - (x & 31)))) {
				if (xL < 0) xL = x;
				xR = x;
			}
		}
	};

	std::vector<TVector2d> leftEdge, rightEdge;
	for (int my = firstOpaqueRow; my <= lastOpaqueRow; ++my) {
		int xL, xR;
		rowEnds(my, xL, xR);
		if (xL < 0) continue;  // gap row — skip
		const float uL = static_cast<float>(xL) / kMaskW;
		const float uR = static_cast<float>(xR + 1) / kMaskW;
		const float v  = static_cast<float>(kMaskH - my) / kMaskH;
		leftEdge.emplace_back(uL, v);
		rightEdge.emplace_back(uR, v);
	}
	sil.contourUV.swap(leftEdge);
	sil.contourUV.insert(sil.contourUV.end(), rightEdge.rbegin(), rightEdge.rend());
}

// --------------------------------------------------------------------
//                       narrow-phase test
// --------------------------------------------------------------------

bool TestTreeSilhouette(const TreeSilhouette& sil,
                        const TVector3d& pos,
                        const TVector3d& treePos,
                        double diam, double height,
                        double tuxRadius) {
	if (sil.W == 0) return false;

	// Billboard plane normal is +Z (axis-aligned; the renderer's 1° Y
	// rotation when perf_level>1 skews the plane by <=1.75% of r — under
	// one mask texel on a 4 m tree — and is absorbed by the broad phase).
	const double dx = pos.x - treePos.x;
	const double dy = pos.y - treePos.y;
	const double dz = pos.z - treePos.z;

	// Sphere ∩ plane: if Tux's sphere can't reach the plane, miss.
	if (std::fabs(dz) > tuxRadius) return false;
	const double discR = std::sqrt(std::max(0.0, tuxRadius * tuxRadius - dz * dz));

	const double u = dx / diam + 0.5;   // 0..1 left..right
	const double v = dy / height;       // 0..1 bottom..top
	const double padU = discR / diam;
	const double padV = discR / height;

	const int w = sil.W;
	const int h = sil.H;
	const int wpr = (w + 31) / 32;

	// Integer bbox: center texel ± ceil(pad in texels). The +1 gives a
	// conservative slack texel on each side, avoiding floor/ceil. Clamp
	// to mask bounds; empty overlap → miss.
	const int cu = static_cast<int>(u * w);
	const int cv = static_cast<int>(v * h);
	const int du = static_cast<int>(padU * w) + 1;
	const int dv = static_cast<int>(padV * h) + 1;

	int x0 = std::max(0, cu - du);
	int x1 = std::min(w - 1, cu + du);
	int y0 = std::max(0, cv - dv);
	int y1 = std::min(h - 1, cv + dv);
	if (x0 > x1 || y0 > y1) return false;

	// V-flip: PNG row 0 (top of `rows[]`) is the top of the tree (V=1).
	for (int y = y0; y <= y1; ++y) {
		const int maskY = h - 1 - y;
		const std::size_t rowOff = static_cast<std::size_t>(maskY) * wpr;
		for (int x = x0; x <= x1; ++x) {
			if (sil.rows[rowOff + (x >> 5)] & (1u << (31 - (x & 31))))
				return true;
		}
	}
	return false;
}
