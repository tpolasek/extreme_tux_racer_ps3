/* PS3 GLU shim — minimal declarations for the symbols ETR uses.
 * On PS3 the quadric/sphere helpers are no-ops (3D path not implemented);
 * gluPerspective is honored (sets the projection matrix), gluErrorString
 * returns a placeholder so check_gl_error() stays quiet.
 */
#ifndef PS3_GL_SHIM_GLU_H
#define PS3_GL_SHIM_GLU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <GL/gl.h>

typedef struct GLUquadric GLUquadricObj;

#define GLU_FILL    0x2B02
#define GLU_SMOOTH  0x2B03
#define GLU_OUTSIDE 0x2B04

GLUquadricObj *gluNewQuadric(void);
void gluDeleteQuadric(GLUquadricObj *q);
void gluQuadricDrawStyle(GLUquadricObj *q, GLenum style);
void gluQuadricOrientation(GLUquadricObj *q, GLenum orientation);
void gluQuadricNormals(GLUquadricObj *q, GLenum normals);

void gluSphere(GLUquadricObj *q, GLdouble radius, GLint slices, GLint stacks);

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble znear, GLdouble zfar);

const GLubyte *gluErrorString(GLenum error);

#ifdef __cplusplus
}
#endif

#endif /* PS3_GL_SHIM_GLU_H */
