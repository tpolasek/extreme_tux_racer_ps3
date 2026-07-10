/* --------------------------------------------------------------------
EXTREME TUXRACER - native replacement types

Internal GL helper: a scoped y-down ortho projection set up from the
current GL viewport, used by the 2D drawables so they are independent
of the caller's matrix state.
---------------------------------------------------------------------*/
#ifndef N_GLUTIL_H
#define N_GLUTIL_H

#include <GL/gl.h>

namespace etr_gl {

struct OrthoGuard {
	GLint vp[4];
	OrthoGuard() {
		glGetIntegerv(GL_VIEWPORT, vp);
		glPushAttrib(GL_ENABLE_BIT | GL_TEXTURE_BIT | GL_CURRENT_BIT | GL_TRANSFORM_BIT);
		glMatrixMode(GL_PROJECTION);
		glPushMatrix();
		glLoadIdentity();
		glOrtho(vp[0], vp[0] + vp[2], vp[1] + vp[3], vp[1], -1.0, 1.0);
		glMatrixMode(GL_MODELVIEW);
		glPushMatrix();
		glLoadIdentity();
	}
	~OrthoGuard() {
		glPopMatrix();
		glMatrixMode(GL_PROJECTION);
		glPopMatrix();
		glPopAttrib();
	}
	unsigned int width() const noexcept  { return static_cast<unsigned int>(vp[2]); }
	unsigned int height() const noexcept { return static_cast<unsigned int>(vp[3]); }
};

} // namespace etr_gl

#endif // N_GLUTIL_H
