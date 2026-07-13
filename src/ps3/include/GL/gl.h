/* PS3 GL fixed-function emulation shim — public header.
 *
 * Provides the GL types, enumerants and function declarations that the ETR
 * game sources reference via <GL/gl.h>. Implementations live in the ps3_gl*
 * sources; only the 2D/immediate-mode + texture + matrix paths are fully
 * implemented, the 3D fixed-function calls (lighting/fog/texgen/stencil) are
 * accepted no-ops so the game links and boots.
 */
#ifndef PS3_GL_SHIM_GL_H
#define PS3_GL_SHIM_GL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* ---- scalar types ---- */
typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef signed char    GLbyte;
typedef short          GLshort;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLubyte;
typedef unsigned short GLushort;
typedef unsigned int   GLuint;
typedef float          GLfloat;
typedef float          GLclampf;
typedef double         GLdouble;
typedef double         GLclampd;
typedef void           GLvoid;

/* ---- boolean / primitives ---- */
#define GL_FALSE              0x0000
#define GL_TRUE               0x0001
#define GL_POINTS             0x0000
#define GL_LINES              0x0001
#define GL_LINE_LOOP          0x0002
#define GL_LINE_STRIP         0x0003
#define GL_TRIANGLES          0x0004
#define GL_TRIANGLE_STRIP     0x0005
#define GL_TRIANGLE_FAN       0x0006
#define GL_QUADS              0x0007
#define GL_QUAD_STRIP         0x0008
#define GL_POLYGON            0x0009

/* ---- data types ---- */
#define GL_BYTE              0x1400
#define GL_UNSIGNED_BYTE     0x1401
#define GL_SHORT             0x1402
#define GL_UNSIGNED_SHORT    0x1403
#define GL_INT               0x1404
#define GL_UNSIGNED_INT      0x1405
#define GL_FLOAT             0x1406
#define GL_DOUBLE            0x140A

/* ---- matrix modes ---- */
#define GL_MODELVIEW         0x1700
#define GL_PROJECTION        0x1701
#define GL_TEXTURE           0x1702

/* ---- gets ---- */
#define GL_VENDOR            0x1F00
#define GL_RENDERER          0x1F01
#define GL_VERSION           0x1F02
#define GL_EXTENSIONS        0x1F03
#define GL_DOUBLEBUFFER      0x0C32
#define GL_RED_BITS          0x0C52
#define GL_GREEN_BITS        0x0C53
#define GL_BLUE_BITS         0x0C54
#define GL_ALPHA_BITS        0x0D55
#define GL_DEPTH_BITS        0x0D56
#define GL_STENCIL_BITS      0x0D57
#define GL_MAX_LIGHTS        0x0D31
#define GL_MAX_TEXTURE_SIZE  0x0D33
#define GL_MAX_MODELVIEW_STACK_DEPTH   0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH  0x0D38
#define GL_MAX_ATTRIB_STACK_DEPTH      0x0D35
#define GL_VIEWPORT          0x0BA2
#define GL_MATRIX_MODE       0x0BA0
#define GL_MODELVIEW_STACK_DEPTH   0x0BA3
#define GL_PROJECTION_STACK_DEPTH  0x0BA4
#define GL_MODELVIEW_MATRIX   0x0BA6
#define GL_PROJECTION_MATRIX  0x0BA7
#define GL_CURRENT_COLOR         0x0B00
#define GL_COLOR_CLEAR_VALUE     0x0C22
#define GL_DEPTH_CLEAR_VALUE     0x0B73
#define GL_DEPTH_WRITEMASK       0x0B72
#define GL_SCISSOR_BOX      0x0C10
#define GL_BLEND_SRC        0x0BE1
#define GL_BLEND_DST        0x0BE0

/* ---- errors ---- */
#define GL_NO_ERROR          0x0000

/* ---- enable caps ---- */
#define GL_TEXTURE_2D              0x0DE1
#define GL_CULL_FACE               0x0B44
#define GL_DEPTH_TEST              0x0B71
#define GL_LIGHTING                0x0B50
#define GL_NORMALIZE               0x0BA1
#define GL_ALPHA_TEST              0x0BC0
#define GL_BLEND                   0x0BE2
#define GL_STENCIL_TEST            0x0B90
#define GL_SCISSOR_TEST            0x0C11
#define GL_TEXTURE_GEN_S           0x0C60
#define GL_TEXTURE_GEN_T           0x0C61
#define GL_COLOR_MATERIAL          0x0B57
#define GL_FOG                     0x0B60

/* ---- client state ---- */
#define GL_VERTEX_ARRAY            0x8074
#define GL_NORMAL_ARRAY            0x8075
#define GL_COLOR_ARRAY             0x8076
#define GL_TEXTURE_COORD_ARRAY     0x8078

/* ---- clear bits ---- */
#define GL_DEPTH_BUFFER_BIT        0x00000100
#define GL_STENCIL_BUFFER_BIT      0x00000400
#define GL_COLOR_BUFFER_BIT        0x00004000

/* ---- attrib bits ---- */
#define GL_CURRENT_BIT             0x00000001
#define GL_ENABLE_BIT              0x00000800
#define GL_TEXTURE_BIT             0x00040000
#define GL_TRANSFORM_BIT           0x00001000
#define GL_ALL_ATTRIB_BITS         0x000FFFFF

/* ---- depth / shade / alpha / stencil ---- */
#define GL_NEVER    0x0200
#define GL_LESS     0x0201
#define GL_EQUAL    0x0202
#define GL_LEQUAL   0x0203
#define GL_GREATER  0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL   0x0206
#define GL_ALWAYS   0x0207
#define GL_SMOOTH   0x1D00
#define GL_FLAT     0x1D01
#define GL_KEEP     0x1E00
#define GL_INCR     0x1E02

/* ---- blend ---- */
#define GL_ZERO                     0x0000
#define GL_ONE                      0x0001
#define GL_SRC_COLOR                0x0300
#define GL_ONE_MINUS_SRC_COLOR      0x0301
#define GL_SRC_ALPHA                0x0302
#define GL_ONE_MINUS_SRC_ALPHA      0x0303
#define GL_DST_ALPHA                0x0304
#define GL_ONE_MINUS_DST_ALPHA      0x0305
#define GL_DST_COLOR                0x0306
#define GL_ONE_MINUS_DST_COLOR      0x0307
#define GL_SRC_ALPHA_SATURATE       0x0308

/* ---- lighting ---- */
#define GL_LIGHT0 0x4000
#define GL_LIGHT1 0x4001
#define GL_LIGHT2 0x4002
#define GL_LIGHT3 0x4003
#define GL_AMBIENT             0x1200
#define GL_DIFFUSE             0x1201
#define GL_SPECULAR            0x1202
#define GL_POSITION            0x1203
#define GL_AMBIENT_AND_DIFFUSE 0x1602
#define GL_SHININESS           0x1601
#define GL_FRONT_AND_BACK      0x0408

/* ---- fog ---- */
#define GL_FOG_MODE  0x0B65
#define GL_FOG_COLOR 0x0B66
#define GL_FOG_START 0x0B63
#define GL_FOG_END   0x0B64
#define GL_FOG_HINT  0x0C54
#define GL_LINEAR    0x2601
#define GL_FASTEST   0x1101
#define GL_NICEST    0x1102

/* ---- texgen ---- */
#define GL_S              0x2000
#define GL_T              0x2001
#define GL_TEXTURE_GEN_MODE 0x2500
#define GL_OBJECT_LINEAR     0x2401
#define GL_OBJECT_PLANE      0x2501

/* ---- texture ---- */
#define GL_TEXTURE_ENV         0x2300
#define GL_TEXTURE_ENV_MODE    0x2200
#define GL_MODULATE            0x2100
#define GL_DECAL               0x2101
#define GL_NEAREST                  0x2600
#define GL_NEAREST_MIPMAP_NEAREST   0x2700
#define GL_LINEAR_MIPMAP_NEAREST    0x2701
#define GL_NEAREST_MIPMAP_LINEAR    0x2702
#define GL_LINEAR_MIPMAP_LINEAR     0x2703
#define GL_TEXTURE_MAG_FILTER  0x2800
#define GL_TEXTURE_MIN_FILTER  0x2801
#define GL_TEXTURE_WRAP_S      0x2802
#define GL_TEXTURE_WRAP_T      0x2803
#define GL_REPEAT              0x2901
#define GL_CLAMP_TO_EDGE       0x812F
#define GL_UNPACK_ALIGNMENT    0x0CF5
#define GL_RGBA                0x1908
#define GL_RGBA8               0x8058
#define GL_TEXTURE_WIDTH       0x1000
#define GL_TEXTURE_HEIGHT      0x1001

/* ---- hint target ---- */

/* =====================================================================
 * Function declarations
 * ===================================================================== */

/* state */
void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glEnableClientState(GLenum cap);
void glDisableClientState(GLenum cap);
GLboolean glIsEnabled(GLenum cap);

/* clear */
void glClear(GLbitfield mask);
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void glClearStencil(GLint s);
void glDepthMask(GLboolean flag);
void glDepthFunc(GLenum func);
void glShadeModel(GLenum mode);
void glBlendFunc(GLenum sfactor, GLenum dfactor);

/* matrix */
void glMatrixMode(GLenum mode);
void glPushMatrix(void);
void glPopMatrix(void);
void glLoadIdentity(void);
void glLoadMatrixd(const GLdouble *m);
void glMultMatrixd(const GLdouble *m);
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glTranslated(GLdouble x, GLdouble y, GLdouble z);
void glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z);
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);

/* attrib stack */
void glPushAttrib(GLbitfield mask);
void glPopAttrib(void);

/* immediate mode */
void glBegin(GLenum mode);
void glEnd(void);
void glVertex2f(GLfloat x, GLfloat y);
void glVertex3d(GLdouble x, GLdouble y, GLdouble z);
void glTexCoord2f(GLfloat s, GLfloat t);
void glTexCoord2d(GLdouble s, GLdouble t);
void glNormal3d(GLdouble x, GLdouble y, GLdouble z);
void glNormal3i(GLint x, GLint y, GLint z);
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a);
void glColor4ubv(const GLubyte *v);

/* arrays */
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p);
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p);
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *p);
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);

/* textures */
void glGenTextures(GLsizei n, GLuint *textures);
void glDeleteTextures(GLsizei n, const GLuint *textures);
void glBindTexture(GLenum target, GLuint texture);
void glTexImage2D(GLenum target, GLint level, GLint internalFormat,
                  GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid *data);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexEnvf(GLenum target, GLenum pname, GLfloat param);
void glTexGeni(GLenum coord, GLenum pname, GLint param);
void glTexGenfv(GLenum coord, GLenum pname, const GLfloat *params);
void glPixelStorei(GLenum pname, GLint param);

/* lighting / material / fog (3D no-ops) */
void glLightfv(GLenum light, GLenum pname, const GLfloat *params);
void glMaterialf(GLenum face, GLenum pname, GLfloat param);
void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);

/* fog (no-op) */
void glFogi(GLenum pname, GLint param);
void glFogf(GLenum pname, GLfloat param);
void glFogfv(GLenum pname, const GLfloat *params);
void glHint(GLenum target, GLenum mode);

/* alpha / stencil (no-op) */
void glAlphaFunc(GLenum func, GLclampf ref);
void glStencilFunc(GLenum func, GLint ref, GLuint mask);
void glStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass);

/* queries */
GLenum glGetError(void);
const GLubyte *glGetString(GLenum name);
void glGetIntegerv(GLenum pname, GLint *params);
void glGetFloatv(GLenum pname, GLfloat *params);
void glGetBooleanv(GLenum pname, GLboolean *params);
void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params);

/* misc */
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, GLvoid *data);

/* compiled vertex array extension (game looks these up via glXGetProcAddressARB;
 * declared here so the PFN typedefs resolve). */
typedef void (*PFNGLLOCKARRAYSEXTPROC)(GLint first, GLsizei count);
typedef void (*PFNGLUNLOCKARRAYSEXTPROC)(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3_GL_SHIM_GL_H */
