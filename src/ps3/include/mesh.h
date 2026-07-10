#ifndef __MESH_H__
#define __MESH_H__

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <vectormath/cpp/vectormath_aos.h>
#include <math.h>

using namespace Vectormath::Aos;

struct S3DVertex
{
	S3DVertex() {}
	S3DVertex(float x, float y, float z, float nx, float ny, float nz, float tu, float tv)
		: pos(x,y,z), nrm(nx,ny,nz), u(tu), v(tv) {}

	Vector3 pos;
	Vector3 nrm;
	float u, v;
};

struct SMeshBuffer
{
	SMeshBuffer() : indices(NULL), indices32(NULL), use32(false),
	                cnt_indices(0), vertices(NULL), cnt_vertices(0) {}

	u16 *indices;
	u32 *indices32;     // 32-bit index path (T0 spike / terrain)
	bool use32;         // true -> draw via indices32 with GCM_INDEX_TYPE_32B
	u32 cnt_indices;
	S3DVertex *vertices;
	u32 cnt_vertices;
};

inline SMeshBuffer* createSphere(float radius, u32 polyCntX, u32 polyCntY)
{
	u32 i, p1, p2, level;
	u32 x, y, polyCntXpitch;
	const float RECIPROCAL_PI = 1.0f / M_PI;
	SMeshBuffer *buffer = new SMeshBuffer();

	if (polyCntX < 2) polyCntX = 2;
	if (polyCntY < 2) polyCntY = 2;
	if (polyCntX * polyCntY > 32767) {
		if (polyCntX > polyCntY)
			polyCntX = 32767 / polyCntY - 1;
		else
			polyCntY = 32767 / (polyCntX + 1);
	}
	polyCntXpitch = polyCntX + 1;

	buffer->cnt_vertices = (polyCntXpitch * polyCntY) + 2;
	buffer->vertices = (S3DVertex*)rsxMemalign(128, buffer->cnt_vertices * sizeof(S3DVertex));

	buffer->cnt_indices = (polyCntX * polyCntY) * 6;
	buffer->indices = (u16*)rsxMemalign(128, buffer->cnt_indices * sizeof(u16));

	i = 0;
	level = 0;
	for (p1 = 0; p1 < polyCntY - 1; p1++) {
		for (p2 = 0; p2 < polyCntX - 1; p2++) {
			const u32 curr = level + p2;
			buffer->indices[i++] = curr;
			buffer->indices[i++] = curr + polyCntXpitch;
			buffer->indices[i++] = curr + 1 + polyCntXpitch;
			buffer->indices[i++] = curr;
			buffer->indices[i++] = curr + 1 + polyCntXpitch;
			buffer->indices[i++] = curr + 1;
		}
		buffer->indices[i++] = level + polyCntX;
		buffer->indices[i++] = level + polyCntX - 1;
		buffer->indices[i++] = level + polyCntX - 1 + polyCntXpitch;
		buffer->indices[i++] = level + polyCntX;
		buffer->indices[i++] = level + polyCntX - 1 + polyCntXpitch;
		buffer->indices[i++] = level + polyCntX + polyCntXpitch;
		level += polyCntXpitch;
	}

	const u32 polyCntSq = polyCntXpitch * polyCntY;
	const u32 polyCntSq1 = polyCntSq + 1;
	const u32 polyCntSqM1 = (polyCntY - 1) * polyCntXpitch;

	for (p2 = 0; p2 < polyCntX - 1; p2++) {
		buffer->indices[i++] = polyCntSq;
		buffer->indices[i++] = p2;
		buffer->indices[i++] = p2 + 1;
		buffer->indices[i++] = polyCntSq1;
		buffer->indices[i++] = polyCntSqM1 + p2;
		buffer->indices[i++] = polyCntSqM1 + p2 + 1;
	}
	buffer->indices[i++] = polyCntSq;
	buffer->indices[i++] = polyCntX - 1;
	buffer->indices[i++] = polyCntX;
	buffer->indices[i++] = polyCntSq1;
	buffer->indices[i++] = polyCntSqM1;
	buffer->indices[i++] = polyCntSqM1 + polyCntX - 1;

	float axz;
	float ay = 0;
	const float angelX = 2 * M_PI / polyCntX;
	const float angelY = M_PI / polyCntY;

	i = 0;
	for (y = 0; y < polyCntY; y++) {
		axz = 0;
		ay += angelY;
		const float sinay = sinf(ay);
		for (x = 0; x < polyCntX; x++) {
			const Vector3 pos(radius * cosf(axz) * sinay, radius * cosf(ay), radius * sinf(axz) * sinay);
			Vector3 normal = normalize(pos);
			float tu = 0.5f;
			if (y == 0) {
				if (normal.getY() != -1.0f && normal.getY() != 1.0f)
					tu = acosf(fmaxf(fminf(normal.getX() / sinay, 1.0f), -1.0f)) * 0.5f * RECIPROCAL_PI;
				if (normal.getZ() < 0.0f) tu = 1 - tu;
			} else {
				tu = buffer->vertices[i - polyCntXpitch].u;
			}
			buffer->vertices[i] = S3DVertex(pos.getX(), pos.getY(), pos.getZ(),
				normal.getX(), normal.getY(), normal.getZ(),
				tu, ay * RECIPROCAL_PI);
			axz += angelX;
			i++;
		}
		buffer->vertices[i] = S3DVertex(buffer->vertices[i - polyCntX]);
		buffer->vertices[i].u = 1.0f;
		i++;
	}
	buffer->vertices[i++] = S3DVertex(0, radius, 0, 0, 1, 0, 0.5f, 0);
	buffer->vertices[i] = S3DVertex(0, -radius, 0, 0, -1, 0, 0.5f, 1);

	return buffer;
}

inline SMeshBuffer* createQuad(float size)
{
	SMeshBuffer *buffer = new SMeshBuffer();
	buffer->cnt_vertices = 4;
	buffer->vertices = (S3DVertex*)rsxMemalign(128, 4 * sizeof(S3DVertex));
	buffer->cnt_indices = 6;
	buffer->indices = (u16*)rsxMemalign(128, 6 * sizeof(u16));

	float h = size * 0.5f;
	buffer->vertices[0] = S3DVertex(-h, 0, -h, 0, 1, 0, 0, 0);
	buffer->vertices[1] = S3DVertex( h, 0, -h, 0, 1, 0, 1, 0);
	buffer->vertices[2] = S3DVertex( h, 0,  h, 0, 1, 0, 1, 1);
	buffer->vertices[3] = S3DVertex(-h, 0,  h, 0, 1, 0, 0, 1);

	buffer->indices[0] = 0; buffer->indices[1] = 1; buffer->indices[2] = 2;
	buffer->indices[3] = 0; buffer->indices[4] = 2; buffer->indices[5] = 3;

	return buffer;
}

// T0 spike: a side x side vertex grid that exceeds the 16-bit index ceiling
// (side=300 -> 90,000 verts, > 65,535) to validate GCM_INDEX_TYPE_32B.
inline SMeshBuffer* createBigGrid(u32 side)
{
	SMeshBuffer *buffer = new SMeshBuffer();
	buffer->use32 = true;

	buffer->cnt_vertices = side * side;
	buffer->vertices = (S3DVertex*)rsxMemalign(128, buffer->cnt_vertices * sizeof(S3DVertex));

	const u32 cells = side - 1;
	buffer->cnt_indices = cells * cells * 6;
	buffer->indices32 = (u32*)rsxMemalign(128, buffer->cnt_indices * sizeof(u32));

	const float span = 60.0f;        // world units across the full grid
	const float half = span * 0.5f;
	const float step = span / cells;
	// UVs span [0,1] so the shared CLAMP_TO_EDGE texture (setTexture) doesn't
	// smear edge texels; the 8x8 checker stretches cleanly across the whole grid.
	const float tileRepeat = 1.0f;

	u32 v = 0;
	for (u32 row = 0; row < side; row++) {
		for (u32 col = 0; col < side; col++) {
			float x = -half + col * step;
			float z = -half + row * step;
			float u = (float)col / cells * tileRepeat;
			float vv = (float)row / cells * tileRepeat;
			buffer->vertices[v++] = S3DVertex(x, 0.0f, z, 0.0f, 1.0f, 0.0f, u, vv);
		}
	}

	u32 i = 0;
	for (u32 row = 0; row < cells; row++) {
		for (u32 col = 0; col < cells; col++) {
			u32 curr  = row * side + col;
			u32 right = curr + 1;
			u32 down  = curr + side;
			u32 diag  = down + 1;
			buffer->indices32[i++] = curr;
			buffer->indices32[i++] = down;
			buffer->indices32[i++] = diag;
			buffer->indices32[i++] = curr;
			buffer->indices32[i++] = diag;
			buffer->indices32[i++] = right;
		}
	}

	return buffer;
}

#endif
