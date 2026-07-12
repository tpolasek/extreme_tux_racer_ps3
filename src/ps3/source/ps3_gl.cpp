/* PS3 fixed-function GL emulation shim (2D path) for ETR.
 *
 * Translates the subset of fixed-function GL the game's 2D menu path uses into
 * RSX commands behind a single Cg program (etr2d). Everything is drawn as
 * immediate-mode GL_QUADS (Sprite / RectangleShape / Text all use
 * glBegin/glVertex2f/glTexCoord2f). 3D fixed-function calls (lighting, fog,
 * texgen, stencil, vertex arrays) are accepted so the game links and boots but
 * are otherwise no-ops.
 *
 * Matrices are stored column-major (OpenGL native) and uploaded transposed to
 * the shader, matching the proven convention from src/ps3/source/main.cpp.
 */
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glx.h>

#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <rsx/mm.h>
#include <io/pad.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "rsxutil.h"
#include "ps3_gl_internal.h"
#include "ps3_tty.h"

/* ---- embedded shader objects (bin2o from etr2d.vcg/.fcg) ---- */
extern "C" const u8 etr2d_vpo[];
extern "C" const u8 etr2d_fpo[];

/* forward declaration: flushes the accumulated immediate-mode vertices as a
 * single RSX draw (defined below). */
extern "C" void ps3_gl_flush(void);

/* =====================================================================
 * Internal state
 * ===================================================================== */

#define PS3_MATRIX_STACK_DEPTH 32
#define PS3_ATTRIB_STACK_DEPTH 16

struct MatrixStack {
	float m[16];
	float stack[PS3_MATRIX_STACK_DEPTH][16];
	int   top;
};

static MatrixStack g_proj;          /* GL_PROJECTION */
static MatrixStack g_mv;            /* GL_MODELVIEW */
static GLenum g_matrixMode = GL_MODELVIEW;

static inline MatrixStack *activeMatrix() {
	return (g_matrixMode == GL_PROJECTION) ? &g_proj : &g_mv;
}

/* tracked enable / state */
static GLboolean g_tex2d       = GL_FALSE;
static GLboolean g_blend       = GL_FALSE;
static GLboolean g_depthTest   = GL_FALSE;
static GLboolean g_cullFace    = GL_FALSE;
static GLboolean g_lighting    = GL_FALSE;
static GLboolean g_alphaTest   = GL_FALSE;
static GLboolean g_stencilTest = GL_FALSE;
static GLboolean g_depthMask   = GL_TRUE;
static GLenum    g_blendSrc    = GL_SRC_ALPHA;
static GLenum    g_blendDst    = GL_ONE_MINUS_SRC_ALPHA;
static float     g_color[4]    = {1.0f, 1.0f, 1.0f, 1.0f};
static GLint     g_viewport[4] = {0, 0, 1280, 720};

/* immediate mode */
struct ImmVtx { float x, y, z, u, v; };
#define PS3_IMM_MAX 4096
static ImmVtx  g_immVtx[PS3_IMM_MAX];
static int     g_immCount = 0;
static GLenum  g_primMode = GL_TRIANGLES;
static int     g_inBegin  = 0;
static float   g_curU = 0.0f, g_curV = 0.0f;

/* draw vertex buffer (RSX-visible, CPU-written) */
static ImmVtx *g_drawBuf = NULL;
static u32     g_drawBufOffset = 0;

/* textures: 1-based ids, index 0 = none */
#define PS3_MAX_TEXTURES 512
struct GlTex {
	u32       offset;
	u32       width, height;
	u32       pitch;      /* bytes per row, 64-byte aligned for RSX linear texturing */
	u8       *buffer;     /* RSX-local backing store */
	GLboolean smooth;
	GLboolean repeated;
	GLboolean used;
};
static GlTex  g_tex[PS3_MAX_TEXTURES];
static GLuint g_currentTex = 0;
static GLuint g_nextTexId  = 1;

/* recycled texture ids (freed by glDeleteTextures, reused by glGenTextures) */
static GLuint g_freeIds[PS3_MAX_TEXTURES];
static u32    g_freeCount = 0;

/* shader handles */
static rsxVertexProgram  *g_vpo = NULL;
static void              *g_vpUcode = NULL;
static rsxFragmentProgram* g_fpo = NULL;
static void              *g_fpUcode = NULL;
static u32               *g_fpBuf = NULL;
static u32                g_fpOffset = 0;
static rsxProgramConst   *g_uProj = NULL;
static rsxProgramConst   *g_uMV   = NULL;
static rsxProgramConst   *g_uColor = NULL;
static rsxProgramAttrib  *g_texSampler = NULL;

/* white 1x1 fallback texture */
static u32 *g_whiteBuf = NULL;
static u32  g_whiteOffset = 0;

/* attrib stack (full tracked-state snapshot) */
struct AttribSave {
	GLboolean tex2d, blend, depthTest, cullFace, lighting, alphaTest, stencilTest, depthMask;
	GLenum blendSrc, blendDst;
	float  color[4];
	GLuint currentTex;
	GLboolean tex2dEnableSaved;
};
static AttribSave g_attrib[PS3_ATTRIB_STACK_DEPTH];
static int        g_attribTop = 0;

/* RSX init done flag */
static int g_rsxReady = 0;

/* =====================================================================
 * Matrix math (column-major)
 * ===================================================================== */
static void matIdentity(float *m) {
	memset(m, 0, sizeof(float) * 16);
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* result = a * b  (column-major; apply b then a) */
static void matMul(float *out, const float *a, const float *b) {
	float t[16];
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			float s = 0.0f;
			for (int k = 0; k < 4; k++)
				s += a[k * 4 + row] * b[col * 4 + k];
			t[col * 4 + row] = s;
		}
	}
	memcpy(out, t, sizeof(float) * 16);
}

static void matTranspose(float *out, const float *m) {
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			out[i * 4 + j] = m[j * 4 + i];
}

/* =====================================================================
 * GL: matrix stack
 * ===================================================================== */
void glMatrixMode(GLenum mode) { g_matrixMode = mode; }

void glPushMatrix(void) {
	MatrixStack *s = activeMatrix();
	if (s->top < PS3_MATRIX_STACK_DEPTH) {
		memcpy(s->stack[s->top], s->m, sizeof(float) * 16);
		s->top++;
	}
}

void glPopMatrix(void) {
	MatrixStack *s = activeMatrix();
	if (s->top > 0) {
		s->top--;
		memcpy(s->m, s->stack[s->top], sizeof(float) * 16);
	}
}

void glLoadIdentity(void) { matIdentity(activeMatrix()->m); }

void glLoadMatrixd(const GLdouble *m) {
	for (int i = 0; i < 16; i++) activeMatrix()->m[i] = (float)m[i];
}

void glMultMatrixd(const GLdouble *m) {
	float mf[16];
	for (int i = 0; i < 16; i++) mf[i] = (float)m[i];
	MatrixStack *s = activeMatrix();
	float tmp[16];
	matMul(tmp, s->m, mf);
	memcpy(s->m, tmp, sizeof(float) * 16);
}

void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
	float m[16];
	matIdentity(m);
	m[0]  = 2.0f / (float)(r - l);
	m[5]  = 2.0f / (float)(t - b);
	m[10] = -2.0f / (float)(f - n);
	m[12] = -(float)(r + l) / (float)(r - l);
	m[13] = -(float)(t + b) / (float)(t - b);
	m[14] = -(float)(f + n) / (float)(f - n);
	MatrixStack *s = activeMatrix();
	float tmp[16];
	matMul(tmp, s->m, m);
	memcpy(s->m, tmp, sizeof(float) * 16);
}

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
	float m[16];
	memset(m, 0, sizeof(float) * 16);
	m[0]  = (float)(2.0 * n / (r - l));
	m[5]  = (float)(2.0 * n / (t - b));
	m[8]  = (float)((r + l) / (r - l));
	m[9]  = (float)((t + b) / (t - b));
	m[10] = (float)(-(f + n) / (f - n));
	m[11] = -1.0f;
	m[14] = (float)(-2.0 * f * n / (f - n));
	MatrixStack *s = activeMatrix();
	float tmp[16];
	matMul(tmp, s->m, m);
	memcpy(s->m, tmp, sizeof(float) * 16);
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
	float m[16];
	matIdentity(m);
	m[12] = x; m[13] = y; m[14] = z;
	MatrixStack *s = activeMatrix();
	float tmp[16];
	matMul(tmp, s->m, m);
	memcpy(s->m, tmp, sizeof(float) * 16);
}

void glTranslated(GLdouble x, GLdouble y, GLdouble z) { glTranslatef((GLfloat)x, (GLfloat)y, (GLfloat)z); }

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
	float c = cosf(angle * (3.14159265f / 180.0f));
	float s = sinf(angle * (3.14159265f / 180.0f));
	float len = sqrtf(x * x + y * y + z * z);
	if (len == 0.0f) return;
	x /= len; y /= len; z /= len;
	float m[16];
	memset(m, 0, sizeof(float) * 16);
	m[0]  = x*x*(1-c)+c;     m[4]  = x*y*(1-c)-z*s;  m[8]  = x*z*(1-c)+y*s;  m[12] = 0;
	m[1]  = y*x*(1-c)+z*s;   m[5]  = y*y*(1-c)+c;    m[9]  = y*z*(1-c)-x*s;  m[13] = 0;
	m[2]  = x*z*(1-c)-y*s;   m[6]  = y*z*(1-c)+x*s;  m[10] = z*z*(1-c)+c;    m[14] = 0;
	m[3]  = 0;               m[7]  = 0;              m[11] = 0;              m[15] = 1;
	MatrixStack *stk = activeMatrix();
	float tmp[16];
	matMul(tmp, stk->m, m);
	memcpy(stk->m, tmp, sizeof(float) * 16);
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
	g_viewport[0] = x; g_viewport[1] = y; g_viewport[2] = w; g_viewport[3] = h;
}

/* =====================================================================
 * GL: state / attrib stack
 * ===================================================================== */
static void snapshotState(AttribSave *a) {
	a->tex2d = g_tex2d; a->blend = g_blend; a->depthTest = g_depthTest;
	a->cullFace = g_cullFace; a->lighting = g_lighting; a->alphaTest = g_alphaTest;
	a->stencilTest = g_stencilTest; a->depthMask = g_depthMask;
	a->blendSrc = g_blendSrc; a->blendDst = g_blendDst;
	memcpy(a->color, g_color, sizeof(float) * 4);
	a->currentTex = g_currentTex;
}

static void restoreState(const AttribSave *a) {
	g_tex2d = a->tex2d; g_blend = a->blend; g_depthTest = a->depthTest;
	g_cullFace = a->cullFace; g_lighting = a->lighting; g_alphaTest = a->alphaTest;
	g_stencilTest = a->stencilTest; g_depthMask = a->depthMask;
	g_blendSrc = a->blendSrc; g_blendDst = a->blendDst;
	memcpy(g_color, a->color, sizeof(float) * 4);
	g_currentTex = a->currentTex;
}

void glPushAttrib(GLbitfield) {
	if (g_attribTop < PS3_ATTRIB_STACK_DEPTH)
		snapshotState(&g_attrib[g_attribTop++]);
}

void glPopAttrib(void) {
	if (g_attribTop > 0)
		restoreState(&g_attrib[--g_attribTop]);
}

void glEnable(GLenum cap) {
	switch (cap) {
		case GL_TEXTURE_2D:     g_tex2d = GL_TRUE; break;
		case GL_BLEND:          g_blend = GL_TRUE; break;
		case GL_DEPTH_TEST:     g_depthTest = GL_TRUE; break;
		case GL_CULL_FACE:      g_cullFace = GL_TRUE; break;
		case GL_LIGHTING:       g_lighting = GL_TRUE; break;
		case GL_ALPHA_TEST:     g_alphaTest = GL_TRUE; break;
		case GL_STENCIL_TEST:   g_stencilTest = GL_TRUE; break;
		default: break;
	}
}

void glDisable(GLenum cap) {
	switch (cap) {
		case GL_TEXTURE_2D:     g_tex2d = GL_FALSE; break;
		case GL_BLEND:          g_blend = GL_FALSE; break;
		case GL_DEPTH_TEST:     g_depthTest = GL_FALSE; break;
		case GL_CULL_FACE:      g_cullFace = GL_FALSE; break;
		case GL_LIGHTING:       g_lighting = GL_FALSE; break;
		case GL_ALPHA_TEST:     g_alphaTest = GL_FALSE; break;
		case GL_STENCIL_TEST:   g_stencilTest = GL_FALSE; break;
		default: break;
	}
}

GLboolean glIsEnabled(GLenum cap) {
	switch (cap) {
		case GL_TEXTURE_2D:   return g_tex2d;
		case GL_BLEND:        return g_blend;
		case GL_DEPTH_TEST:   return g_depthTest;
		case GL_CULL_FACE:    return g_cullFace;
		case GL_LIGHTING:     return g_lighting;
		default:              return GL_FALSE;
	}
}

void glEnableClientState(GLenum)  { }
void glDisableClientState(GLenum) { }

void glDepthMask(GLboolean f) { g_depthMask = f; }
void glDepthFunc(GLenum)      { }
void glShadeModel(GLenum)     { }
void glAlphaFunc(GLenum, GLclampf) { }
void glStencilFunc(GLenum, GLint, GLuint) { }
void glStencilOp(GLenum, GLenum, GLenum) { }

void glBlendFunc(GLenum sf, GLenum df) { g_blendSrc = sf; g_blendDst = df; }

/* lighting / material / fog / hint : no-ops (3D) */
void glLightfv(GLenum, GLenum, const GLfloat*)    { }
void glMaterialf(GLenum, GLenum, GLfloat)         { }
void glMaterialfv(GLenum, GLenum, const GLfloat*) { }
void glFogi(GLenum, GLint)         { }
void glFogf(GLenum, GLfloat)       { }
void glFogfv(GLenum, const GLfloat*) { }
void glHint(GLenum, GLenum)        { }

/* texgen : no-op (3D) */
void glTexGeni(GLenum, GLenum, GLint)         { }
void glTexGenfv(GLenum, GLenum, const GLfloat*) { }

/* =====================================================================
 * GL: immediate mode
 * ===================================================================== */
static GLenum mapPrim(GLenum p) {
	switch (p) {
		case GL_QUADS:          return GCM_TYPE_QUADS;
		case GL_TRIANGLES:      return GCM_TYPE_TRIANGLES;
		case GL_TRIANGLE_FAN:   return GCM_TYPE_TRIANGLE_FAN;
		case GL_TRIANGLE_STRIP: return GCM_TYPE_TRIANGLE_STRIP;
		case GL_QUAD_STRIP:     return GCM_TYPE_QUAD_STRIP;
		case GL_LINES:          return GCM_TYPE_LINES;
		case GL_LINE_STRIP:     return GCM_TYPE_LINE_STRIP;
		case GL_POINTS:         return GCM_TYPE_POINTS;
		default:                return GCM_TYPE_TRIANGLES;
	}
}

void glBegin(GLenum mode) {
	g_primMode = mode;
	g_immCount = 0;
	g_inBegin = 1;
}

void glEnd(void) {
	g_inBegin = 0;
	if (g_immCount > 0) ps3_gl_flush();
	g_immCount = 0;
}

void glVertex2f(GLfloat x, GLfloat y) {
	if (!g_inBegin || g_immCount >= PS3_IMM_MAX) return;
	ImmVtx &v = g_immVtx[g_immCount++];
	v.x = x; v.y = y; v.z = 0.0f; v.u = g_curU; v.v = g_curV;
}

void glVertex3d(GLdouble x, GLdouble y, GLdouble z) {
	if (!g_inBegin || g_immCount >= PS3_IMM_MAX) return;
	ImmVtx &v = g_immVtx[g_immCount++];
	v.x = (float)x; v.y = (float)y; v.z = (float)z; v.u = g_curU; v.v = g_curV;
}

void glTexCoord2f(GLfloat s, GLfloat t) { g_curU = s; g_curV = t; }
void glTexCoord2d(GLdouble s, GLdouble t) { g_curU = (float)s; g_curV = (float)t; }

void glNormal3d(GLdouble, GLdouble, GLdouble) { }
void glNormal3i(GLint, GLint, GLint)          { }

void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
	g_color[0] = r; g_color[1] = g; g_color[2] = b; g_color[3] = a;
}

void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) {
	static const float inv = 1.0f / 255.0f;
	g_color[0] = r * inv; g_color[1] = g * inv; g_color[2] = b * inv; g_color[3] = a * inv;
}

void glColor4ubv(const GLubyte *v) { glColor4ub(v[0], v[1], v[2], v[3]); }

/* =====================================================================
 * GL: vertex arrays (3D — no-ops)
 * ===================================================================== */
void glVertexPointer(GLint, GLenum, GLsizei, const GLvoid*)  { }
void glTexCoordPointer(GLint, GLenum, GLsizei, const GLvoid*) { }
void glNormalPointer(GLenum, GLsizei, const GLvoid*)         { }
void glColorPointer(GLint, GLenum, GLsizei, const GLvoid*)   { }
void glDrawArrays(GLenum, GLint, GLsizei)                    { }
void glDrawElements(GLenum, GLsizei, GLenum, const GLvoid*)  { }

/* =====================================================================
 * GL: textures
 * ===================================================================== */
void glGenTextures(GLsizei n, GLuint *t) {
	for (GLsizei i = 0; i < n; i++) {
		if (g_freeCount > 0) t[i] = g_freeIds[--g_freeCount];
		else                 t[i] = g_nextTexId++;
	}
}

void glDeleteTextures(GLsizei n, const GLuint *t) {
	for (GLsizei i = 0; i < n; i++) {
		GLuint id = t[i];
		if (id > 0 && id < PS3_MAX_TEXTURES) {
			GlTex &T = g_tex[id];
			/* return RSX backing store to its heap (libc free() would leak) */
			if (T.buffer) rsxFree(T.buffer);
			T.buffer = NULL;
			T.offset = 0;
			T.width = T.height = T.pitch = 0;
			T.used = GL_FALSE;
			if (g_freeCount < PS3_MAX_TEXTURES) g_freeIds[g_freeCount++] = id;
		}
	}
}

void glBindTexture(GLenum, GLuint t) {
	if (t > 0 && t < PS3_MAX_TEXTURES) g_currentTex = t;
	else if (t == 0) g_currentTex = 0;
}

void glTexImage2D(GLenum, GLint, GLint, GLsizei w, GLsizei h, GLint,
                  GLenum, GLenum, const GLvoid *data) {
	if (g_currentTex == 0 || g_currentTex >= PS3_MAX_TEXTURES) return;
	GlTex &T = g_tex[g_currentTex];
	T.used = GL_TRUE;
	T.width = w; T.height = h;

	/* RSX linear textures require pitch to be a multiple of 64 bytes; pad
	 * each row accordingly. The source row stride is still w*4 (tight RGBA). */
	const u32 srcRowBytes = (u32)w * 4u;
	const u32 pitch = (srcRowBytes + 63u) & ~63u;
	T.pitch = pitch;

	/* (Re)allocate backing store. RGBA8, swizzled to ARGB on upload.
	 * rsxFree handles memory from rsxMemalign; libc free() leaks it. */
	if (T.buffer) rsxFree(T.buffer);
	T.buffer = (u8 *)rsxMemalign(128, (u32)pitch * (u32)h);
	if (!T.buffer) { sysTtyTrace("[etr] glTexImage2D: rsxMemalign FAILED\n"); return; }

	const u8 *src = (const u8 *)data;
	if (src) {
		for (GLsizei y = 0; y < h; y++) {
			const u8 *srow = src + y * srcRowBytes;
			u8 *drow = T.buffer + (u32)y * pitch;
			for (GLsizei x = 0; x < w; x++) {
				u32 di = (u32)x * 4u;
				u32 si = (u32)x * 4u;
				drow[di + 1] = srow[si + 0]; /* R */
				drow[di + 2] = srow[si + 1]; /* G */
				drow[di + 3] = srow[si + 2]; /* B */
				drow[di + 0] = srow[si + 3]; /* A */
			}
			/* zero-fill trailing alignment padding in this row */
			if (pitch > srcRowBytes)
				memset(drow + srcRowBytes, 0, pitch - srcRowBytes);
		}
	} else {
		memset(T.buffer, 0, (u32)pitch * (u32)h);
	}

	/* PPU data cache flush so RSX sees the writes, and invalidate the RSX
	 * texture cache so a re-used buffer offset is not sampled with stale
	 * texels from a previously resident texture (fixes "first upload fine,
	 * subsequent uploads corrupted" on per-frame font text). */
	asm volatile("sync");
	rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
	rsxAddressToOffset(T.buffer, &T.offset);
}

void glTexParameteri(GLenum, GLenum pname, GLint param) {
	if (g_currentTex == 0 || g_currentTex >= PS3_MAX_TEXTURES) return;
	GlTex &T = g_tex[g_currentTex];
	switch (pname) {
		case GL_TEXTURE_MIN_FILTER:
		case GL_TEXTURE_MAG_FILTER: T.smooth = (param == GL_LINEAR || param == GL_LINEAR_MIPMAP_LINEAR || param == GL_NEAREST_MIPMAP_LINEAR) ? GL_TRUE : GL_FALSE; break;
		case GL_TEXTURE_WRAP_S:
		case GL_TEXTURE_WRAP_T:     T.repeated = (param == GL_REPEAT) ? GL_TRUE : GL_FALSE; break;
		default: break;
	}
}

void glTexEnvf(GLenum, GLenum, GLfloat) { }
void glPixelStorei(GLenum, GLint)       { }

/* =====================================================================
 * GL: clear
 * ===================================================================== */
static GLclampf g_clearR = 0, g_clearG = 0, g_clearB = 0, g_clearA = 0;
static GLint    g_clearStencil = 0;

void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
	g_clearR = r; g_clearG = g; g_clearB = b; g_clearA = a;
}
void glClearStencil(GLint s) { g_clearStencil = s; }

void glClear(GLbitfield) {
	u32 col = ((u32)(g_clearA * 255.0f) & 0xFF) << 24 |
	          ((u32)(g_clearR * 255.0f) & 0xFF) << 16 |
	          ((u32)(g_clearG * 255.0f) & 0xFF) << 8  |
	          ((u32)(g_clearB * 255.0f) & 0xFF);
	rsxSetClearColor(context, col);
	rsxSetClearDepthStencil(context, 0xffffff00u | (g_clearStencil & 0xFF));
	rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
	                        GCM_CLEAR_S | GCM_CLEAR_Z);
}

/* =====================================================================
 * GL: queries
 * ===================================================================== */
GLenum glGetError(void) { return GL_NO_ERROR; }

static const char *g_vendor = "PSL1GHT";
static const char *g_renderer = "PS3 RSX (ETR GL shim)";
static const char *g_version = "1.2 ETR-PS3";
static const char *g_extensions = "";
const GLubyte *glGetString(GLenum name) {
	switch (name) {
		case GL_VENDOR:     return (const GLubyte *)g_vendor;
		case GL_RENDERER:   return (const GLubyte *)g_renderer;
		case GL_VERSION:    return (const GLubyte *)g_version;
		case GL_EXTENSIONS: return (const GLubyte *)g_extensions;
		default:            return (const GLubyte *)"";
	}
}

void glGetIntegerv(GLenum pname, GLint *p) {
	switch (pname) {
		case GL_VIEWPORT:
			p[0] = g_viewport[0]; p[1] = g_viewport[1];
			p[2] = g_viewport[2]; p[3] = g_viewport[3];
			break;
		case GL_MAX_TEXTURE_SIZE:            *p = 4096; break;
		case GL_MAX_LIGHTS:                  *p = 8;    break;
		case GL_MAX_MODELVIEW_STACK_DEPTH:   *p = PS3_MATRIX_STACK_DEPTH; break;
		case GL_MAX_PROJECTION_STACK_DEPTH:  *p = PS3_MATRIX_STACK_DEPTH; break;
		case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: *p = 8;  break;
		case GL_DEPTH_BITS:                  *p = 24; break;
		case GL_STENCIL_BITS:                *p = 8;  break;
		case GL_DOUBLEBUFFER:                *p = GL_TRUE; break;
		default:                             *p = 0;  break;
	}
}

void glGetFloatv(GLenum, GLfloat *p) { *p = 0.0f; }
void glGetBooleanv(GLenum, GLboolean *p) { *p = GL_FALSE; }

void glGetTexLevelParameteriv(GLenum, GLint, GLenum pname, GLint *p) {
	if (g_currentTex && g_currentTex < PS3_MAX_TEXTURES) {
		GlTex &T = g_tex[g_currentTex];
		if (pname == GL_TEXTURE_WIDTH)  *p = T.width;
		else if (pname == GL_TEXTURE_HEIGHT) *p = T.height;
		else *p = 0;
	} else *p = 0;
}

void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*) { }

/* =====================================================================
 * Internal: setup, draw env, texture bind, flush
 * ===================================================================== */

/* GLX stubs (only glXGetProcAddressARB is actually called). Declared in our
 * <GL/glx.h> as extern "C", so the definitions must match that linkage. */
extern "C" void *glXGetProcAddressARB(const GLubyte *) { return NULL; }
extern "C" int   glXChooseFBConfig(void *, int, const int *, int *) { return 0; }
extern "C" void *glXGetVisualFromFBConfig(void *, GLXFBConfig) { return NULL; }
extern "C" GLXContext glXCreateNewContext(void *, GLXFBConfig, int, GLXContext, GLboolean) { return NULL; }
extern "C" void  glXDestroyContext(void *, GLXContext) { }
extern "C" int   glXMakeCurrent(void *, unsigned long, GLXContext) { return 1; }
extern "C" void  glXSwapBuffers(void *, unsigned long) { }

void ps3_gl_init(void) {
	if (g_rsxReady) return;
	sysTtyTrace("[etr] ps3_gl_init: loading shaders\n");

	g_vpo = (rsxVertexProgram *)etr2d_vpo;
	u32 vpsz = 0;
	rsxVertexProgramGetUCode(g_vpo, &g_vpUcode, &vpsz);
	g_uProj = rsxVertexProgramGetConst(g_vpo, "projMatrix");
	g_uMV   = rsxVertexProgramGetConst(g_vpo, "modelViewMatrix");

	g_fpo = (rsxFragmentProgram *)etr2d_fpo;
	u32 fpsz = 0;
	rsxFragmentProgramGetUCode(g_fpo, &g_fpUcode, &fpsz);
	g_fpBuf = (u32 *)rsxMemalign(64, fpsz);
	memcpy(g_fpBuf, g_fpUcode, fpsz);
	rsxAddressToOffset(g_fpBuf, &g_fpOffset);
	g_uColor     = rsxFragmentProgramGetConst(g_fpo, "uColor");
	g_texSampler = rsxFragmentProgramGetAttrib(g_fpo, "tex");

	if (!g_uProj) sysTtyTrace("[etr] ps3_gl_init: WARN projMatrix const missing\n");
	if (!g_uMV)   sysTtyTrace("[etr] ps3_gl_init: WARN modelViewMatrix const missing\n");
	if (!g_uColor)sysTtyTrace("[etr] ps3_gl_init: WARN uColor const missing\n");

	/* white 1x1 fallback texture (ARGB 0xFFFFFFFF) */
	g_whiteBuf = (u32 *)rsxMemalign(128, 4);
	g_whiteBuf[0] = 0xFFFFFFFFu;
	rsxAddressToOffset(g_whiteBuf, &g_whiteOffset);

	/* draw vertex buffer (CPU-written, RSX-read) */
	g_drawBuf = (ImmVtx *)rsxMemalign(64, sizeof(ImmVtx) * PS3_IMM_MAX);
	rsxAddressToOffset(g_drawBuf, &g_drawBufOffset);

	matIdentity(g_proj.m);
	matIdentity(g_mv.m);
	g_proj.top = g_mv.top = 0;
	g_viewport[2] = display_width;
	g_viewport[3] = display_height;

	/* initial render target (first frame draws before any flip) */
	setRenderTarget(curr_fb);

	g_rsxReady = 1;
	sysTtyTrace("[etr] ps3_gl_init: ready\n");
}

static void setDrawEnv(void) {
	u16 x = 0, y = 0;
	u16 w = display_width, h = display_height;
	float min = 0.0f, max = 1.0f;
	float scale[4], offset[4];
	scale[0] = w * 0.5f;  scale[1] = h * -0.5f;  scale[2] = (max - min) * 0.5f;  scale[3] = 0.0f;
	offset[0] = x + w * 0.5f;  offset[1] = y + h * 0.5f;  offset[2] = (max + min) * 0.5f;  offset[3] = 0.0f;

	rsxSetColorMask(context, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(context, 0);
	rsxSetViewport(context, x, y, w, h, min, max, scale, offset);
	rsxSetScissor(context, x, y, w, h);
	rsxSetDepthTestEnable(context, g_depthTest ? GCM_TRUE : GCM_FALSE);
	rsxSetDepthFunc(context, GCM_LESS);
	rsxSetDepthWriteEnable(context, g_depthMask ? 1 : 0);
	rsxSetFrontFace(context, GCM_FRONTFACE_CCW);
	rsxSetShadeModel(context, GCM_SHADE_MODEL_SMOOTH);
	rsxSetBlendEnable(context, g_blend ? GCM_TRUE : GCM_FALSE);
	if (g_blend) {
		rsxSetBlendFunc(context, g_blendSrc, g_blendDst, g_blendSrc, g_blendDst);
	}
	for (u8 i = 0; i < 8; i++)
		rsxSetViewportClip(context, i, display_width, display_height);
	rsxSetZMinMaxControl(context, 0, 1, 1);
}

static void bindTextureForDraw(void) {
	gcmTexture texture;
	u32 offset, tw, th, pitch;
	u8  filtMin, filtMag;
	u8  wrapS, wrapT;

	if (g_tex2d && g_currentTex > 0 && g_currentTex < PS3_MAX_TEXTURES && g_tex[g_currentTex].used) {
		GlTex &T = g_tex[g_currentTex];
		offset = T.offset; tw = T.width; th = T.height; pitch = T.pitch;
		u8 f = T.smooth ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST;
		filtMin = filtMag = f;
		wrapS = T.repeated ? GCM_TEXTURE_REPEAT : GCM_TEXTURE_CLAMP_TO_EDGE;
		wrapT = T.repeated ? GCM_TEXTURE_REPEAT : GCM_TEXTURE_CLAMP_TO_EDGE;
	} else {
		offset = g_whiteOffset; tw = 1; th = 1; pitch = 4;
		filtMin = filtMag = GCM_TEXTURE_NEAREST;
		wrapS = wrapT = GCM_TEXTURE_CLAMP_TO_EDGE;
	}

	rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);

	texture.format    = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	texture.mipmap    = 1;
	texture.dimension = GCM_TEXTURE_DIMS_2D;
	texture.cubemap   = GCM_FALSE;
	texture.remap     = (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
	                    (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
	                    (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
	                    (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
	                    (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
	                    (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
	                    (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
	                    (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT);
	texture.width     = tw;
	texture.height    = th;
	texture.depth     = 1;
	texture.location  = GCM_LOCATION_RSX;
	texture.pitch     = pitch;
	texture.offset    = offset;

	rsxLoadTexture(context, g_texSampler ? g_texSampler->index : 0, &texture);
	rsxTextureControl(context, g_texSampler ? g_texSampler->index : 0, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(context, g_texSampler ? g_texSampler->index : 0, 0, filtMag, filtMin, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(context, g_texSampler ? g_texSampler->index : 0, wrapS, wrapT, wrapT, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

extern "C" void ps3_gl_flush(void) {
	if (g_immCount == 0) return;

	/* copy accumulated verts into the RSX-visible draw buffer */
	int n = g_immCount;
	if (n > PS3_IMM_MAX) n = PS3_IMM_MAX;
	memcpy(g_drawBuf, g_immVtx, sizeof(ImmVtx) * n);

	setDrawEnv();
	bindTextureForDraw();

	/* bind POS + TEX0 attribs */
	rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0, g_drawBufOffset,
	                         sizeof(ImmVtx), 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0, g_drawBufOffset + sizeof(float) * 3,
	                         sizeof(ImmVtx), 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

	/* vertex program + matrices (uploaded transposed) */
	rsxLoadVertexProgram(context, g_vpo, g_vpUcode);
	float tmp[16];
	if (g_uProj) { matTranspose(tmp, g_proj.m); rsxSetVertexProgramParameter(context, g_vpo, g_uProj, tmp); }
	if (g_uMV)   { matTranspose(tmp, g_mv.m);   rsxSetVertexProgramParameter(context, g_vpo, g_uMV, tmp); }

	/* fragment program + color uniform */
	rsxLoadFragmentProgramLocation(context, g_fpo, g_fpOffset, GCM_LOCATION_RSX);
	if (g_uColor)
		rsxSetFragmentProgramParameter(context, g_fpo, g_uColor, g_color, g_fpOffset, GCM_LOCATION_RSX);

	rsxSetUserClipPlaneControl(context,
		GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
		GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);

	rsxInvalidateVertexCache(context);
	rsxDrawVertexArray(context, mapPrim(g_primMode), 0, n);
}

/* =====================================================================
 * GLU stubs
 * ===================================================================== */
void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble znear, GLdouble zfar) {
	GLdouble fh = tan(fovy * 3.14159265358979323846 / 360.0) * znear;
	GLdouble fw = fh * aspect;
	glFrustum(-fw, fw, -fh, fh, znear, zfar);
}

const GLubyte *gluErrorString(GLenum) {
	return (const GLubyte *)"PS3 GL shim: no error";
}

/* quadrics: no-op (only tux.cpp's sphere, which is 3D) */
GLUquadricObj *gluNewQuadric(void)                 { return (GLUquadricObj *)1; }
void gluDeleteQuadric(GLUquadricObj*)              { }
void gluQuadricDrawStyle(GLUquadricObj*, GLenum)   { }
void gluQuadricOrientation(GLUquadricObj*, GLenum) { }
void gluQuadricNormals(GLUquadricObj*, GLenum)     { }
void gluSphere(GLUquadricObj*, GLdouble, GLint, GLint) { }
