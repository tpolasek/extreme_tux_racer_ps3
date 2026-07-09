#ifndef __MATHUTILS_H__
#define __MATHUTILS_H__

#include <vectormath/cpp/vectormath_aos.h>
#include <math.h>

using namespace Vectormath::Aos;

inline Matrix4 makePerspective(float fovY, float aspect, float near, float far)
{
	float f = 1.0f / tanf(fovY * 0.5f);
	float nf = 1.0f / (near - far);
	return Matrix4(
		Vector4(f / aspect, 0, 0, 0),
		Vector4(0, f, 0, 0),
		Vector4(0, 0, (far + near) * nf, -1),
		Vector4(0, 0, 2.0f * far * near * nf, 0)
	);
}

inline Matrix4 makeLookAt(const Point3& eye, const Point3& target, const Vector3& up)
{
	Vector3 z = normalize(eye - target);
	Vector3 x = normalize(cross(up, z));
	Vector3 y = cross(z, x);
	return Matrix4(
		Vector4(x.getX(), y.getX(), z.getX(), 0),
		Vector4(x.getY(), y.getY(), z.getY(), 0),
		Vector4(x.getZ(), y.getZ(), z.getZ(), 0),
		Vector4(-dot(Vector3(eye), x), -dot(Vector3(eye), y), -dot(Vector3(eye), z), 1)
	);
}

inline Matrix4 makeRotationX(float radians)
{
	float c = cosf(radians);
	float s = sinf(radians);
	return Matrix4(
		Vector4(1, 0, 0, 0),
		Vector4(0, c, s, 0),
		Vector4(0, -s, c, 0),
		Vector4(0, 0, 0, 1)
	);
}

inline Matrix4 makeRotationY(float radians)
{
	float c = cosf(radians);
	float s = sinf(radians);
	return Matrix4(
		Vector4(c, 0, -s, 0),
		Vector4(0, 1, 0, 0),
		Vector4(s, 0, c, 0),
		Vector4(0, 0, 0, 1)
	);
}

inline Matrix4 makeTranslation(float x, float y, float z)
{
	return Matrix4(
		Vector4(1, 0, 0, 0),
		Vector4(0, 1, 0, 0),
		Vector4(0, 0, 1, 0),
		Vector4(x, y, z, 1)
	);
}

inline Matrix4 makeIdentity()
{
	return Matrix4(
		Vector4(1, 0, 0, 0),
		Vector4(0, 1, 0, 0),
		Vector4(0, 0, 1, 0),
		Vector4(0, 0, 0, 1)
	);
}

#define DEGTORAD(a) ((a) * 0.01745329252f)

#endif
