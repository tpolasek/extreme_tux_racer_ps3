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
	SMeshBuffer() : indices(NULL), cnt_indices(0), vertices(NULL), cnt_vertices(0) {}

	u16 *indices;
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

#endif
