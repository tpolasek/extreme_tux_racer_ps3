/* PS3 fixed-function GL emulation shim for Extreme Tux Racer.
 *
 * Implements the subset of OpenGL fixed-function the game actually calls —
 * both the 2D menu path and the full 3D race path — on top of RSX + a single
 * Cg program (etr3d).
 *
 * Supported:
 *   matrices (modelview / projection stacks, ortho, frustum, load/mult)
 *   immediate mode (quads, triangles, fans, strips, quad-strips)
 *   client vertex / normal / color / texcoord arrays + DrawArrays /
 *     DrawElements (host-memory yanked into the RSX ring each call)
 *   RGBA8 textures (RGBA → A8R8G8B8 linear, 64-byte pitch pad)
 *   blend / depth / cull / alpha-test state
 *   one directional/positional light + material (ambient/diffuse/specular)
 *   linear fog, object-linear texgen (S/T)
 *   gluSphere (expanded into immediate-mode strips)
 *
 * Not implemented (game never relies on them after our stubs):
 *   multi-light accumulation past light0 bilaterally, texenv DECAL vs MODULATE
 *   (MODULATE is the only path), stencil buffer, texture units > 0.
 *
 * Matrices are stored column-major (OpenGL) and uploaded TRANSPOSED to the
 * shader, matching PSL1GHT's Vectormath / cgcomp convention.
 */
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glx.h>

#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <rsx/mm.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "rsxutil.h"
#include "ps3_gl_internal.h"
#include "ps3_tty.h"

/* ---- embedded shader objects (bin2o from etr3d.vcg/.fcg) ---- */
extern "C" const u8 etr3d_vpo[];
extern "C" const u8 etr3d_fpo[];

extern "C" void ps3_gl_flush(void);

/* =====================================================================
 * Internal state
 * ===================================================================== */

#define PS3_MATRIX_STACK_DEPTH 32
#define PS3_ATTRIB_STACK_DEPTH 16
#define PS3_IMM_MAX            4096
#define PS3_NUM_LIGHTS         4

struct MatrixStack {
	float m[16];
	float stack[PS3_MATRIX_STACK_DEPTH][16];
	int   top;
};

static MatrixStack g_proj;
static MatrixStack g_mv;
static GLenum g_matrixMode = GL_MODELVIEW;

static inline MatrixStack *activeMatrix() {
	return (g_matrixMode == GL_PROJECTION) ? &g_proj : &g_mv;
}

/* enable / tracked fixed-function state */
static GLboolean g_tex2d       = GL_FALSE;
static GLboolean g_blend       = GL_FALSE;
static GLboolean g_depthTest   = GL_FALSE;
static GLboolean g_cullFace    = GL_FALSE;
static GLboolean g_lighting    = GL_FALSE;
static GLboolean g_alphaTest   = GL_FALSE;
static GLboolean g_stencilTest = GL_FALSE;
static GLboolean g_fog         = GL_FALSE;
static GLboolean g_texGenS     = GL_FALSE;
static GLboolean g_texGenT     = GL_FALSE;
static GLboolean g_normalize   = GL_FALSE;   /* informational; shader always renorms */
static GLboolean g_depthMask   = GL_TRUE;
static GLenum    g_blendSrc    = GL_SRC_ALPHA;
static GLenum    g_blendDst    = GL_ONE_MINUS_SRC_ALPHA;
static GLenum    g_depthFunc   = GL_LESS;
static GLenum    g_alphaFunc   = GL_GEQUAL;
static GLclampf  g_alphaRef    = 0.5f;
static float     g_color[4]    = {1.f, 1.f, 1.f, 1.f};
static GLint     g_viewport[4] = {0, 0, 1280, 720};

/* material */
static float g_matDiffuse[4]  = {1.f, 1.f, 1.f, 1.f};
static float g_matSpecular[4] = {0.f, 0.f, 0.f, 1.f};
static float g_matShininess   = 1.f;

/* lights — pos is stored already transformed into eye-space at glLightfv
 * time (OpenGL multiplies POSITION by the current modelview when the light
 * is set). isDir is true when the original w component was 0. */
struct Light {
	float posEye[3];   /* eye-space position, or direction when isDir */
	float ambient[4];
	float diffuse[4];
	float specular[4];
	GLboolean isDir;
	GLboolean enabled;
};
static Light g_light[PS3_NUM_LIGHTS];

/* fog (linear only — the sole mode the game uses) */
static float g_fogColor[4] = {0.9f, 0.9f, 1.f, 0.f};
static float g_fogStart = 20.f;
static float g_fogEnd   = 70.f;

/* object-linear texgen planes */
static float g_texPlaneS[4] = {1.f, 0.f, 0.f, 0.f};
static float g_texPlaneT[4] = {0.f, 0.f, 1.f, 0.f};

/* ---- immediate mode / flush vertex ----
 * Layout matches GCM attrib binds in layout order:
 *   POS(3f) + NRM(3f) + TEX0(2f) + COL(4f)  = 12 floats / 48 bytes
 */
struct ImmVtx {
	float x, y, z;
	float nx, ny, nz;
	float u, v;
	float r, g, b, a;
};
static ImmVtx  g_immVtx[PS3_IMM_MAX];
static int     g_immCount = 0;
static GLenum  g_primMode = GL_TRIANGLES;
static int     g_inBegin  = 0;
static float   g_curU = 0.f, g_curV = 0.f;
static float   g_curNx = 0.f, g_curNy = 1.f, g_curNz = 0.f;

/* Vertex draw ring (fencing prevents CPU overwrite of in-flight draws).
 * Label 253; rsxutil uses 255 for flip/idle. */
#define PS3_VTX_RING      32
#define PS3_VTX_SLOT_MAX  512
#define PS3_VTX_LABEL_IDX 253
struct VtxSlot {
	ImmVtx *buf;
	u32     offset;
	u32     labelVal;
};
static VtxSlot  g_vtxRing[PS3_VTX_RING];
static int      g_vtxRingHead = 0;
static ImmVtx  *g_vtxOversize    = NULL;
static u32      g_vtxOversizeOff = 0;
static vu32    *g_vtxLabel       = NULL;
static u32      g_vtxLabelNext   = 1;

/* textures */
#define PS3_MAX_TEXTURES 512
struct GlTex {
	u32       offset;
	u32       width, height;
	u32       pitch;
	u8       *buffer;
	GLboolean smooth;
	GLboolean repeated;
	GLboolean used;
};
static GlTex  g_tex[PS3_MAX_TEXTURES];
static GLuint g_currentTex = 0;
static GLuint g_nextTexId  = 1;
static GLuint g_freeIds[PS3_MAX_TEXTURES];
static u32    g_freeCount = 0;

/* shader handles (etr3d) */
static rsxVertexProgram   *g_vpo = NULL;
static void               *g_vpUcode = NULL;
static rsxFragmentProgram *g_fpo = NULL;
static void               *g_fpUcode = NULL;
static u32                *g_fpBuf = NULL;
static u32                 g_fpOffset = 0;

static rsxProgramConst *g_uProj = NULL;
static rsxProgramConst *g_uMV = NULL;
static rsxProgramConst *g_uTexPlaneS = NULL;
static rsxProgramConst *g_uTexPlaneT = NULL;
static rsxProgramConst *g_uDoTexGen = NULL;

static rsxProgramConst *g_uGlobalAmbient = NULL;
static rsxProgramConst *g_uLightPos = NULL;
static rsxProgramConst *g_uLightColor = NULL;
static rsxProgramConst *g_uLightSpec = NULL;
static rsxProgramConst *g_uLightIsDir = NULL;
static rsxProgramConst *g_uMatDiffuse = NULL;
static rsxProgramConst *g_uMatSpecular = NULL;
static rsxProgramConst *g_uShininess = NULL;
static rsxProgramConst *g_uDoLighting = NULL;
static rsxProgramConst *g_uFogColor = NULL;
static rsxProgramConst *g_uFogSE = NULL;
static rsxProgramConst *g_uDoFog = NULL;
static rsxProgramConst *g_uAlphaRef = NULL;
static rsxProgramConst *g_uDoAlphaTest = NULL;
static rsxProgramAttrib *g_texSampler = NULL;

/* white 1x1 fallback */
static u32 *g_whiteBuf = NULL;
static u32  g_whiteOffset = 0;

/* clear state */
static GLclampf g_clearR = 0, g_clearG = 0, g_clearB = 0, g_clearA = 0;
static GLint    g_clearStencil = 0;

/* attrib stack */
struct AttribSave {
	GLboolean tex2d, blend, depthTest, cullFace, lighting, alphaTest, stencilTest;
	GLboolean fog, texGenS, texGenT, depthMask;
	GLenum blendSrc, blendDst, depthFunc, alphaFunc;
	GLclampf alphaRef;
	float color[4];
	float matDiffuse[4], matSpecular[4], matShininess;
	GLuint currentTex;
};
static AttribSave g_attrib[PS3_ATTRIB_STACK_DEPTH];
static int        g_attribTop = 0;

static int g_rsxReady = 0;

/* =====================================================================
 * Matrix math (column-major)
 * ===================================================================== */
static void matIdentity(float *m) {
	memset(m, 0, sizeof(float) * 16);
	m[0] = m[5] = m[10] = m[15] = 1.f;
}

static void matMul(float *out, const float *a, const float *b) {
	float t[16];
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			float s = 0.f;
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

/* Transform a vec4 by column-major m → out. */
static void matMulVec4(float *out, const float *m, const float *v) {
	out[0] = m[0]*v[0] + m[4]*v[1] + m[8] *v[2] + m[12]*v[3];
	out[1] = m[1]*v[0] + m[5]*v[1] + m[9] *v[2] + m[13]*v[3];
	out[2] = m[2]*v[0] + m[6]*v[1] + m[10]*v[2] + m[14]*v[3];
	out[3] = m[3]*v[0] + m[7]*v[1] + m[11]*v[2] + m[15]*v[3];
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
	m[0]  = 2.f / (float)(r - l);
	m[5]  = 2.f / (float)(t - b);
	m[10] = -2.f / (float)(f - n);
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
	m[11] = -1.f;
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
void glTranslated(GLdouble x, GLdouble y, GLdouble z) {
	glTranslatef((GLfloat)x, (GLfloat)y, (GLfloat)z);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
	float c = cosf(angle * (3.14159265f / 180.f));
	float s = sinf(angle * (3.14159265f / 180.f));
	float len = sqrtf(x * x + y * y + z * z);
	if (len == 0.f) return;
	x /= len; y /= len; z /= len;
	float m[16];
	memset(m, 0, sizeof(float) * 16);
	m[0]  = x*x*(1-c)+c;   m[4]  = x*y*(1-c)-z*s; m[8]  = x*z*(1-c)+y*s;
	m[1]  = y*x*(1-c)+z*s; m[5]  = y*y*(1-c)+c;   m[9]  = y*z*(1-c)-x*s;
	m[2]  = x*z*(1-c)-y*s; m[6]  = y*z*(1-c)+x*s; m[10] = z*z*(1-c)+c;
	m[15] = 1.f;
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
	a->stencilTest = g_stencilTest; a->fog = g_fog;
	a->texGenS = g_texGenS; a->texGenT = g_texGenT; a->depthMask = g_depthMask;
	a->blendSrc = g_blendSrc; a->blendDst = g_blendDst;
	a->depthFunc = g_depthFunc; a->alphaFunc = g_alphaFunc; a->alphaRef = g_alphaRef;
	memcpy(a->color, g_color, sizeof(float) * 4);
	memcpy(a->matDiffuse, g_matDiffuse, sizeof(float) * 4);
	memcpy(a->matSpecular, g_matSpecular, sizeof(float) * 4);
	a->matShininess = g_matShininess;
	a->currentTex = g_currentTex;
}

static void restoreState(const AttribSave *a) {
	g_tex2d = a->tex2d; g_blend = a->blend; g_depthTest = a->depthTest;
	g_cullFace = a->cullFace; g_lighting = a->lighting; g_alphaTest = a->alphaTest;
	g_stencilTest = a->stencilTest; g_fog = a->fog;
	g_texGenS = a->texGenS; g_texGenT = a->texGenT; g_depthMask = a->depthMask;
	g_blendSrc = a->blendSrc; g_blendDst = a->blendDst;
	g_depthFunc = a->depthFunc; g_alphaFunc = a->alphaFunc; g_alphaRef = a->alphaRef;
	memcpy(g_color, a->color, sizeof(float) * 4);
	memcpy(g_matDiffuse, a->matDiffuse, sizeof(float) * 4);
	memcpy(g_matSpecular, a->matSpecular, sizeof(float) * 4);
	g_matShininess = a->matShininess;
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

static int lightIndex(GLenum cap) {
	if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + PS3_NUM_LIGHTS)
		return (int)(cap - GL_LIGHT0);
	return -1;
}

void glEnable(GLenum cap) {
	int li;
	switch (cap) {
		case GL_TEXTURE_2D:     g_tex2d = GL_TRUE; break;
		case GL_BLEND:          g_blend = GL_TRUE; break;
		case GL_DEPTH_TEST:     g_depthTest = GL_TRUE; break;
		case GL_CULL_FACE:      g_cullFace = GL_TRUE; break;
		case GL_LIGHTING:       g_lighting = GL_TRUE; break;
		case GL_ALPHA_TEST:     g_alphaTest = GL_TRUE; break;
		case GL_STENCIL_TEST:   g_stencilTest = GL_TRUE; break;
		case GL_FOG:            g_fog = GL_TRUE; break;
		case GL_TEXTURE_GEN_S:  g_texGenS = GL_TRUE; break;
		case GL_TEXTURE_GEN_T:  g_texGenT = GL_TRUE; break;
		case GL_NORMALIZE:      g_normalize = GL_TRUE; break;
		case GL_COLOR_MATERIAL: break; /* material always tracks glColor lightpath via vColor */
		default:
			if ((li = lightIndex(cap)) >= 0) g_light[li].enabled = GL_TRUE;
			break;
	}
}

void glDisable(GLenum cap) {
	int li;
	switch (cap) {
		case GL_TEXTURE_2D:     g_tex2d = GL_FALSE; break;
		case GL_BLEND:          g_blend = GL_FALSE; break;
		case GL_DEPTH_TEST:     g_depthTest = GL_FALSE; break;
		case GL_CULL_FACE:      g_cullFace = GL_FALSE; break;
		case GL_LIGHTING:       g_lighting = GL_FALSE; break;
		case GL_ALPHA_TEST:     g_alphaTest = GL_FALSE; break;
		case GL_STENCIL_TEST:   g_stencilTest = GL_FALSE; break;
		case GL_FOG:            g_fog = GL_FALSE; break;
		case GL_TEXTURE_GEN_S:  g_texGenS = GL_FALSE; break;
		case GL_TEXTURE_GEN_T:  g_texGenT = GL_FALSE; break;
		case GL_NORMALIZE:      g_normalize = GL_FALSE; break;
		default:
			if ((li = lightIndex(cap)) >= 0) g_light[li].enabled = GL_FALSE;
			break;
	}
}

GLboolean glIsEnabled(GLenum cap) {
	int li;
	switch (cap) {
		case GL_TEXTURE_2D:   return g_tex2d;
		case GL_BLEND:        return g_blend;
		case GL_DEPTH_TEST:   return g_depthTest;
		case GL_CULL_FACE:    return g_cullFace;
		case GL_LIGHTING:     return g_lighting;
		case GL_FOG:          return g_fog;
		default:
			if ((li = lightIndex(cap)) >= 0) return g_light[li].enabled;
			return GL_FALSE;
	}
}

void glDepthMask(GLboolean f) { g_depthMask = f; }
void glDepthFunc(GLenum f)    { g_depthFunc = f; }
void glShadeModel(GLenum)     { }
void glAlphaFunc(GLenum f, GLclampf r) { g_alphaFunc = f; g_alphaRef = r; }
void glStencilFunc(GLenum, GLint, GLuint) { }
void glStencilOp(GLenum, GLenum, GLenum)  { }
void glBlendFunc(GLenum sf, GLenum df)    { g_blendSrc = sf; g_blendDst = df; }

/* =====================================================================
 * GL: lighting / material / fog / texgen
 * ===================================================================== */
void glLightfv(GLenum light, GLenum pname, const GLfloat *p) {
	int li = lightIndex(light);
	if (li < 0 || !p) return;
	Light &L = g_light[li];
	switch (pname) {
		case GL_POSITION: {
			/* OpenGL freezes light position into eye-space under the CURRENT
			 * modelview (camera already applied when Env.SetupLight runs). */
			float eye[4];
			matMulVec4(eye, g_mv.m, p);
			if (p[3] == 0.f) {
				L.posEye[0] = eye[0]; L.posEye[1] = eye[1]; L.posEye[2] = eye[2];
				L.isDir = GL_TRUE;
			} else {
				float iw = (eye[3] != 0.f) ? (1.f / eye[3]) : 1.f;
				L.posEye[0] = eye[0] * iw;
				L.posEye[1] = eye[1] * iw;
				L.posEye[2] = eye[2] * iw;
				L.isDir = GL_FALSE;
			}
			break;
		}
		case GL_AMBIENT:  memcpy(L.ambient,  p, 4 * sizeof(float)); break;
		case GL_DIFFUSE:  memcpy(L.diffuse,  p, 4 * sizeof(float)); break;
		case GL_SPECULAR: memcpy(L.specular, p, 4 * sizeof(float)); break;
		default: break;
	}
}

void glMaterialf(GLenum, GLenum pname, GLfloat v) {
	if (pname == GL_SHININESS) g_matShininess = v;
}

void glMaterialfv(GLenum, GLenum pname, const GLfloat *p) {
	if (!p) return;
	switch (pname) {
		case GL_AMBIENT_AND_DIFFUSE:
		case GL_DIFFUSE:  memcpy(g_matDiffuse,  p, 4 * sizeof(float)); break;
		case GL_SPECULAR: memcpy(g_matSpecular, p, 4 * sizeof(float)); break;
		case GL_AMBIENT:  /* absorbed into light ambient; ignore material ambient */ break;
		default: break;
	}
}

void glFogi(GLenum pname, GLint v) {
	(void)pname; (void)v; /* only GL_LINEAR is supported */
}
void glFogf(GLenum pname, GLfloat v) {
	if (pname == GL_FOG_START) g_fogStart = v;
	else if (pname == GL_FOG_END) g_fogEnd = v;
}
void glFogfv(GLenum pname, const GLfloat *p) {
	if (pname == GL_FOG_COLOR && p) memcpy(g_fogColor, p, 4 * sizeof(float));
}
void glHint(GLenum, GLenum) { }

void glTexGeni(GLenum, GLenum, GLint) { /* only OBJECT_LINEAR is used */ }
void glTexGenfv(GLenum coord, GLenum pname, const GLfloat *p) {
	if (pname != GL_OBJECT_PLANE || !p) return;
	if (coord == GL_S) memcpy(g_texPlaneS, p, 4 * sizeof(float));
	else if (coord == GL_T) memcpy(g_texPlaneT, p, 4 * sizeof(float));
}

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

static void pushImm(float x, float y, float z) {
	if (g_immCount >= PS3_IMM_MAX) return;
	ImmVtx &v = g_immVtx[g_immCount++];
	v.x = x; v.y = y; v.z = z;
	v.nx = g_curNx; v.ny = g_curNy; v.nz = g_curNz;
	v.u = g_curU; v.v = g_curV;
	v.r = g_color[0]; v.g = g_color[1]; v.b = g_color[2]; v.a = g_color[3];
}

void glVertex2f(GLfloat x, GLfloat y)               { if (g_inBegin) pushImm(x, y, 0.f); }
void glVertex3d(GLdouble x, GLdouble y, GLdouble z) { if (g_inBegin) pushImm((float)x, (float)y, (float)z); }

void glTexCoord2f(GLfloat s, GLfloat t) { g_curU = s; g_curV = t; }
void glTexCoord2d(GLdouble s, GLdouble t) { g_curU = (float)s; g_curV = (float)t; }

void glNormal3d(GLdouble x, GLdouble y, GLdouble z) {
	g_curNx = (float)x; g_curNy = (float)y; g_curNz = (float)z;
}
void glNormal3i(GLint x, GLint y, GLint z) {
	g_curNx = (float)x; g_curNy = (float)y; g_curNz = (float)z;
}

void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
	g_color[0] = r; g_color[1] = g; g_color[2] = b; g_color[3] = a;
}
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) {
	static const float inv = 1.f / 255.f;
	g_color[0] = r * inv; g_color[1] = g * inv; g_color[2] = b * inv; g_color[3] = a * inv;
}
void glColor4ubv(const GLubyte *v) { glColor4ub(v[0], v[1], v[2], v[3]); }

/* =====================================================================
 * GL: client vertex arrays
 * ===================================================================== */
struct ClientArray {
	GLboolean     enabled;
	const GLvoid *pointer;
	GLint         size;
	GLenum        type;
	GLsizei       stride;
};
static ClientArray g_vertexArray   = { GL_FALSE, NULL, 0, 0, 0 };
static ClientArray g_texCoordArray = { GL_FALSE, NULL, 0, 0, 0 };
static ClientArray g_normalArray   = { GL_FALSE, NULL, 0, 0, 0 };
static ClientArray g_colorArray    = { GL_FALSE, NULL, 0, 0, 0 };

void glEnableClientState(GLenum cap) {
	switch (cap) {
		case GL_VERTEX_ARRAY:        g_vertexArray.enabled   = GL_TRUE; break;
		case GL_TEXTURE_COORD_ARRAY: g_texCoordArray.enabled = GL_TRUE; break;
		case GL_NORMAL_ARRAY:        g_normalArray.enabled   = GL_TRUE; break;
		case GL_COLOR_ARRAY:         g_colorArray.enabled    = GL_TRUE; break;
		default: break;
	}
}
void glDisableClientState(GLenum cap) {
	switch (cap) {
		case GL_VERTEX_ARRAY:        g_vertexArray.enabled   = GL_FALSE; break;
		case GL_TEXTURE_COORD_ARRAY: g_texCoordArray.enabled = GL_FALSE; break;
		case GL_NORMAL_ARRAY:        g_normalArray.enabled   = GL_FALSE; break;
		case GL_COLOR_ARRAY:         g_colorArray.enabled    = GL_FALSE; break;
		default: break;
	}
}

static u32 sizeofGlType(GLenum t) {
	switch (t) {
		case GL_FLOAT:         return 4;
		case GL_SHORT:         return 2;
		case GL_INT:           return 4;
		case GL_UNSIGNED_BYTE: return 1;
		case GL_UNSIGNED_INT:  return 4;
		default:               return 4;
	}
}

static float readArrayComp(const ClientArray &a, GLint idx, GLint comp) {
	if (!a.pointer) return 0.f;
	GLsizei effStride = a.stride ? a.stride : (GLsizei)(sizeofGlType(a.type) * a.size);
	const u8 *base = (const u8 *)a.pointer + (u32)idx * (u32)effStride;
	switch (a.type) {
		case GL_FLOAT:         return ((const float *)base)[comp];
		case GL_SHORT:         return (float)((const short *)base)[comp];
		case GL_INT:           return (float)((const int *)base)[comp];
		case GL_UNSIGNED_BYTE: return (float)((const u8 *)base)[comp];
		default:               return 0.f;
	}
}

/* color arrays are GLubyte[4] or float[4] — normalize ubyte to 0..1 */
static void readColor(const ClientArray &a, GLint idx, float *out) {
	if (!a.pointer || !a.enabled) {
		out[0] = g_color[0]; out[1] = g_color[1]; out[2] = g_color[2]; out[3] = g_color[3];
		return;
	}
	GLsizei effStride = a.stride ? a.stride : (GLsizei)(sizeofGlType(a.type) * a.size);
	const u8 *base = (const u8 *)a.pointer + (u32)idx * (u32)effStride;
	if (a.type == GL_UNSIGNED_BYTE) {
		static const float inv = 1.f / 255.f;
		out[0] = base[0] * inv; out[1] = base[1] * inv;
		out[2] = base[2] * inv; out[3] = (a.size > 3) ? base[3] * inv : 1.f;
	} else {
		const float *f = (const float *)base;
		out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
		out[3] = (a.size > 3) ? f[3] : 1.f;
	}
}

static void fetchVertex(GLint idx, ImmVtx &v) {
	if (g_vertexArray.enabled) {
		v.x = readArrayComp(g_vertexArray, idx, 0);
		v.y = (g_vertexArray.size > 1) ? readArrayComp(g_vertexArray, idx, 1) : 0.f;
		v.z = (g_vertexArray.size > 2) ? readArrayComp(g_vertexArray, idx, 2) : 0.f;
	} else {
		v.x = v.y = v.z = 0.f;
	}
	if (g_normalArray.enabled) {
		v.nx = readArrayComp(g_normalArray, idx, 0);
		v.ny = readArrayComp(g_normalArray, idx, 1);
		v.nz = readArrayComp(g_normalArray, idx, 2);
	} else {
		v.nx = g_curNx; v.ny = g_curNy; v.nz = g_curNz;
	}
	if (g_texCoordArray.enabled) {
		v.u = readArrayComp(g_texCoordArray, idx, 0);
		v.v = (g_texCoordArray.size > 1) ? readArrayComp(g_texCoordArray, idx, 1) : 0.f;
	} else {
		v.u = g_curU; v.v = g_curV;
	}
	float c[4];
	readColor(g_colorArray, idx, c);
	v.r = c[0]; v.g = c[1]; v.b = c[2]; v.a = c[3];
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
	g_vertexArray.size = size; g_vertexArray.type = type;
	g_vertexArray.stride = stride; g_vertexArray.pointer = ptr;
}
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
	g_texCoordArray.size = size; g_texCoordArray.type = type;
	g_texCoordArray.stride = stride; g_texCoordArray.pointer = ptr;
}
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr) {
	g_normalArray.size = 3; g_normalArray.type = type;
	g_normalArray.stride = stride; g_normalArray.pointer = ptr;
}
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
	g_colorArray.size = size; g_colorArray.type = type;
	g_colorArray.stride = stride; g_colorArray.pointer = ptr;
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
	if (count <= 0) return;
	/* pull into ImmVtx, flushing as the ring capacity is filled so large
	 * draws never overflow. Primitive restarts (fan/strip) only of primitive
	 * types that are restart-safe from an index of 0: we only split TRIANGLES
	 * and QUADS. For FAN/STRIP force a single flush. */
	bool canSplit = (mode == GL_TRIANGLES || mode == GL_QUADS ||
	                 mode == GL_LINES || mode == GL_POINTS);
	GLint issued = 0;
	while (issued < count) {
		GLsizei batch = count - issued;
		if (canSplit && batch > PS3_VTX_SLOT_MAX) {
			/* keep batch a multiple of the primitive size */
			int step = (mode == GL_QUADS) ? 4 : (mode == GL_TRIANGLES) ? 3 :
			           (mode == GL_LINES) ? 2 : 1;
			batch = (PS3_VTX_SLOT_MAX / step) * step;
			if (batch == 0) batch = step;
		}
		if (batch > PS3_IMM_MAX) batch = PS3_IMM_MAX;
		g_immCount = 0;
		for (GLint i = 0; i < batch; i++)
			fetchVertex(first + issued + i, g_immVtx[g_immCount++]);
		g_primMode = mode;
		ps3_gl_flush();
		issued += batch;
		if (!canSplit) break; /* fan/strip: one shot */
	}
	g_immCount = 0;
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
	if (count <= 0 || !indices) return;
	/* expand fully-indexed draw into de-indexed ImmVtx and reuse DrawArrays
	 * batching path. 32-bit indices are the course's preferred form. */
	GLint issued = 0;
	bool canSplit = (mode == GL_TRIANGLES || mode == GL_QUADS);
	while (issued < count) {
		GLsizei batch = count - issued;
		if (canSplit && batch > PS3_VTX_SLOT_MAX) {
			int step = (mode == GL_QUADS) ? 4 : 3;
			batch = (PS3_VTX_SLOT_MAX / step) * step;
			if (batch == 0) batch = step;
		}
		if (batch > PS3_IMM_MAX) batch = PS3_IMM_MAX;
		g_immCount = 0;
		for (GLsizei i = 0; i < batch; i++) {
			GLuint idx;
			if (type == GL_UNSIGNED_INT)
				idx = ((const GLuint *)indices)[issued + i];
			else if (type == GL_UNSIGNED_SHORT)
				idx = ((const GLushort *)indices)[issued + i];
			else
				idx = ((const GLubyte *)indices)[issued + i];
			fetchVertex((GLint)idx, g_immVtx[g_immCount++]);
		}
		g_primMode = mode;
		ps3_gl_flush();
		issued += batch;
		if (!canSplit) break;
	}
	g_immCount = 0;
}

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

	const u32 srcRowBytes = (u32)w * 4u;
	const u32 pitch = (srcRowBytes + 63u) & ~63u;
	T.pitch = pitch;

	if (T.buffer) rsxFree(T.buffer);
	T.buffer = (u8 *)rsxMemalign(128, (u32)pitch * (u32)h);
	if (!T.buffer) { sysTtyTrace("[etr] glTexImage2D: rsxMemalign FAILED\n"); return; }

	const u8 *src = (const u8 *)data;
	if (src) {
		for (GLsizei y = 0; y < h; y++) {
			const u8 *srow = src + y * srcRowBytes;
			u8 *drow = T.buffer + (u32)y * pitch;
			for (GLsizei x = 0; x < w; x++) {
				u32 di = (u32)x * 4u, si = (u32)x * 4u;
				drow[di + 1] = srow[si + 0]; /* R */
				drow[di + 2] = srow[si + 1]; /* G */
				drow[di + 3] = srow[si + 2]; /* B */
				drow[di + 0] = srow[si + 3]; /* A */
			}
			if (pitch > srcRowBytes)
				memset(drow + srcRowBytes, 0, pitch - srcRowBytes);
		}
	} else {
		memset(T.buffer, 0, (u32)pitch * (u32)h);
	}

	asm volatile("sync");
	rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
	rsxAddressToOffset(T.buffer, &T.offset);
}

void glTexParameteri(GLenum, GLenum pname, GLint param) {
	if (g_currentTex == 0 || g_currentTex >= PS3_MAX_TEXTURES) return;
	GlTex &T = g_tex[g_currentTex];
	switch (pname) {
		case GL_TEXTURE_MIN_FILTER:
		case GL_TEXTURE_MAG_FILTER:
			T.smooth = (param == GL_LINEAR || param == GL_LINEAR_MIPMAP_LINEAR ||
			            param == GL_NEAREST_MIPMAP_LINEAR) ? GL_TRUE : GL_FALSE;
			break;
		case GL_TEXTURE_WRAP_S:
		case GL_TEXTURE_WRAP_T:
			T.repeated = (param == GL_REPEAT) ? GL_TRUE : GL_FALSE;
			break;
		default: break;
	}
}
void glTexEnvf(GLenum, GLenum, GLfloat) { }
void glPixelStorei(GLenum, GLint)       { }

/* =====================================================================
 * GL: clear
 * ===================================================================== */
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
	g_clearR = r; g_clearG = g; g_clearB = b; g_clearA = a;
}
void glClearStencil(GLint s) { g_clearStencil = s; }

void glClear(GLbitfield) {
	if (!g_rsxReady) return;
	u32 col = ((u32)(g_clearA * 255.f) & 0xFF) << 24 |
	          ((u32)(g_clearR * 255.f) & 0xFF) << 16 |
	          ((u32)(g_clearG * 255.f) & 0xFF) << 8  |
	          ((u32)(g_clearB * 255.f) & 0xFF);
	rsxSetClearColor(context, col);
	rsxSetClearDepthStencil(context, 0xffffff00u | (g_clearStencil & 0xFF));
	rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
	                        GCM_CLEAR_S | GCM_CLEAR_Z);
}

/* =====================================================================
 * GL: queries
 * ===================================================================== */
GLenum glGetError(void) { return GL_NO_ERROR; }

static const char *g_vendor     = "PSL1GHT";
static const char *g_renderer   = "PS3 RSX (ETR GL shim)";
static const char *g_version    = "1.2 ETR-PS3";
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
		case GL_MAX_LIGHTS:                  *p = PS3_NUM_LIGHTS; break;
		case GL_MAX_MODELVIEW_STACK_DEPTH:   *p = PS3_MATRIX_STACK_DEPTH; break;
		case GL_MAX_PROJECTION_STACK_DEPTH:  *p = PS3_MATRIX_STACK_DEPTH; break;
		case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: *p = 8; break;
		case GL_DEPTH_BITS:   *p = 24; break;
		case GL_STENCIL_BITS: *p = 8;  break;
		case GL_DOUBLEBUFFER: *p = GL_TRUE; break;
		default:              *p = 0;  break;
	}
}
void glGetFloatv(GLenum, GLfloat *p) { *p = 0.f; }
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

/* GLX stubs */
extern "C" void *glXGetProcAddressARB(const GLubyte *) { return NULL; }
extern "C" int   glXChooseFBConfig(void *, int, const int *, int *) { return 0; }
extern "C" void *glXGetVisualFromFBConfig(void *, GLXFBConfig) { return NULL; }
extern "C" GLXContext glXCreateNewContext(void *, GLXFBConfig, int, GLXContext, GLboolean) { return NULL; }
extern "C" void  glXDestroyContext(void *, GLXContext) { }
extern "C" int   glXMakeCurrent(void *, unsigned long, GLXContext) { return 1; }
extern "C" void  glXSwapBuffers(void *, unsigned long) { }

/* =====================================================================
 * init / draw-env / bind-texture / flush
 * ===================================================================== */
static void initDefaultLight(void) {
	for (int i = 0; i < PS3_NUM_LIGHTS; i++) {
		Light &L = g_light[i];
		memset(&L, 0, sizeof(L));
		L.posEye[0] = 0.f; L.posEye[1] = 0.f; L.posEye[2] = 1.f;
		L.isDir = GL_TRUE;
		L.ambient[0] = L.ambient[1] = L.ambient[2] = 0.f; L.ambient[3] = 1.f;
		L.diffuse[0] = L.diffuse[1] = L.diffuse[2] = L.diffuse[3] = 1.f;
		L.specular[0] = L.specular[1] = L.specular[2] = L.specular[3] = 1.f;
		L.enabled = (i == 0) ? GL_TRUE : GL_FALSE;
	}
}

void ps3_gl_init(void) {
	if (g_rsxReady) return;
	sysTtyTrace("[etr] ps3_gl_init: loading etr3d shaders\n");

	g_vpo = (rsxVertexProgram *)etr3d_vpo;
	u32 vpsz = 0;
	rsxVertexProgramGetUCode(g_vpo, &g_vpUcode, &vpsz);
	g_uProj      = rsxVertexProgramGetConst(g_vpo, "projMatrix");
	g_uMV        = rsxVertexProgramGetConst(g_vpo, "modelViewMatrix");
	g_uTexPlaneS = rsxVertexProgramGetConst(g_vpo, "texPlaneS");
	g_uTexPlaneT = rsxVertexProgramGetConst(g_vpo, "texPlaneT");
	g_uDoTexGen  = rsxVertexProgramGetConst(g_vpo, "doTexGen");

	g_fpo = (rsxFragmentProgram *)etr3d_fpo;
	u32 fpsz = 0;
	rsxFragmentProgramGetUCode(g_fpo, &g_fpUcode, &fpsz);
	g_fpBuf = (u32 *)rsxMemalign(64, fpsz);
	memcpy(g_fpBuf, g_fpUcode, fpsz);
	rsxAddressToOffset(g_fpBuf, &g_fpOffset);

	g_uGlobalAmbient = rsxFragmentProgramGetConst(g_fpo, "globalAmbient");
	g_uLightPos      = rsxFragmentProgramGetConst(g_fpo, "lightPosition");
	g_uLightColor    = rsxFragmentProgramGetConst(g_fpo, "lightColor");
	g_uLightSpec     = rsxFragmentProgramGetConst(g_fpo, "lightSpecular");
	g_uLightIsDir    = rsxFragmentProgramGetConst(g_fpo, "lightIsDir");
	g_uMatDiffuse    = rsxFragmentProgramGetConst(g_fpo, "matDiffuse");
	g_uMatSpecular   = rsxFragmentProgramGetConst(g_fpo, "matSpecular");
	g_uShininess     = rsxFragmentProgramGetConst(g_fpo, "shininess");
	g_uDoLighting    = rsxFragmentProgramGetConst(g_fpo, "doLighting");
	g_uFogColor      = rsxFragmentProgramGetConst(g_fpo, "fogColor");
	g_uFogSE         = rsxFragmentProgramGetConst(g_fpo, "fogSE");
	g_uDoFog         = rsxFragmentProgramGetConst(g_fpo, "doFog");
	g_uAlphaRef      = rsxFragmentProgramGetConst(g_fpo, "alphaRef");
	g_uDoAlphaTest   = rsxFragmentProgramGetConst(g_fpo, "doAlphaTest");
	g_texSampler     = rsxFragmentProgramGetAttrib(g_fpo, "texture");

	if (!g_uProj) sysTtyTrace("[etr] ps3_gl_init: WARN projMatrix missing\n");
	if (!g_uMV)   sysTtyTrace("[etr] ps3_gl_init: WARN modelViewMatrix missing\n");

	g_whiteBuf = (u32 *)rsxMemalign(128, 4);
	g_whiteBuf[0] = 0xFFFFFFFFu;
	rsxAddressToOffset(g_whiteBuf, &g_whiteOffset);

	for (int i = 0; i < PS3_VTX_RING; i++) {
		g_vtxRing[i].buf = (ImmVtx *)rsxMemalign(64, sizeof(ImmVtx) * PS3_VTX_SLOT_MAX);
		if (!g_vtxRing[i].buf) {
			sysTtyTrace("[etr] ps3_gl_init: vtx ring alloc FAILED\n");
			return;
		}
		rsxAddressToOffset(g_vtxRing[i].buf, &g_vtxRing[i].offset);
		g_vtxRing[i].labelVal = 0;
	}
	g_vtxRingHead = 0;
	g_vtxOversize = (ImmVtx *)rsxMemalign(64, sizeof(ImmVtx) * PS3_IMM_MAX);
	rsxAddressToOffset(g_vtxOversize, &g_vtxOversizeOff);

	g_vtxLabel = (vu32 *)gcmGetLabelAddress(PS3_VTX_LABEL_IDX);
	*g_vtxLabel = 0;
	g_vtxLabelNext = 1;

	matIdentity(g_proj.m);
	matIdentity(g_mv.m);
	g_proj.top = g_mv.top = 0;
	g_viewport[2] = display_width;
	g_viewport[3] = display_height;
	initDefaultLight();

	setRenderTarget(curr_fb);
	g_rsxReady = 1;
	sysTtyTrace("[etr] ps3_gl_init: ready (etr3d)\n");
}

static void setDrawEnv(void) {
	u16 x = 0, y = 0;
	u16 w = display_width, h = display_height;
	float min = 0.f, max = 1.f;
	float scale[4], offset[4];
	scale[0] = w * 0.5f;  scale[1] = h * -0.5f;  scale[2] = (max - min) * 0.5f;  scale[3] = 0.f;
	offset[0] = x + w * 0.5f;  offset[1] = y + h * 0.5f;  offset[2] = (max + min) * 0.5f;  offset[3] = 0.f;

	rsxSetColorMask(context, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(context, 0);
	rsxSetViewport(context, x, y, w, h, min, max, scale, offset);
	rsxSetScissor(context, x, y, w, h);

	/* GCM depth-func enums match the GL values we store. */
	rsxSetDepthTestEnable(context, g_depthTest ? GCM_TRUE : GCM_FALSE);
	rsxSetDepthFunc(context, g_depthFunc);
	rsxSetDepthWriteEnable(context, g_depthMask ? 1 : 0);

	rsxSetFrontFace(context, GCM_FRONTFACE_CCW);
	rsxSetCullFaceEnable(context, g_cullFace ? GCM_TRUE : GCM_FALSE);
	if (g_cullFace) rsxSetCullFace(context, GCM_CULL_BACK);
	rsxSetShadeModel(context, GCM_SHADE_MODEL_SMOOTH);

	rsxSetLogicOpEnable(context, GCM_FALSE);
	rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
	rsxSetBlendFunc(context,
	                g_blend ? g_blendSrc : GCM_ONE,
	                g_blend ? g_blendDst : GCM_ZERO,
	                g_blend ? g_blendSrc : GCM_ONE,
	                g_blend ? g_blendDst : GCM_ZERO);
	rsxSetBlendEnable(context, g_blend ? GCM_TRUE : GCM_FALSE);

	/* Hardware alpha test for GEQUAL (trees/particles). Other funcs fall
	 * back to the shader discard path. */
	if (g_alphaTest && g_alphaFunc == GL_GEQUAL) {
		rsxSetAlphaFunc(context, GCM_GEQUAL, (u32)(g_alphaRef * 255.f));
		rsxSetAlphaTestEnable(context, GCM_TRUE);
	} else {
		rsxSetAlphaTestEnable(context, GCM_FALSE);
	}

	for (u8 i = 0; i < 8; i++)
		rsxSetViewportClip(context, i, display_width, display_height);
	rsxSetZMinMaxControl(context, 0, 1, 1);
}

static void bindTextureForDraw(void) {
	gcmTexture texture;
	u32 offset, tw, th, pitch;
	u8  filtMin, filtMag, wrapS, wrapT;

	if (g_tex2d && g_currentTex > 0 && g_currentTex < PS3_MAX_TEXTURES && g_tex[g_currentTex].used) {
		GlTex &T = g_tex[g_currentTex];
		offset = T.offset; tw = T.width; th = T.height; pitch = T.pitch;
		u8 f = T.smooth ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST;
		filtMin = filtMag = f;
		wrapS = T.repeated ? GCM_TEXTURE_REPEAT : GCM_TEXTURE_CLAMP_TO_EDGE;
		wrapT = T.repeated ? GCM_TEXTURE_REPEAT : GCM_TEXTURE_CLAMP_TO_EDGE;
	} else {
		/* unwrap or untextured → 1x1 white so the FP sample is identity */
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

	u8 unit = g_texSampler ? g_texSampler->index : 0;
	rsxLoadTexture(context, unit, &texture);
	rsxTextureControl(context, unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(context, unit, 0, filtMag, filtMin, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(context, unit, wrapS, wrapT, wrapT, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

/* Pick primary light (first enabled; usually light0) + sum ambient. Position is
 * already in eye-space (snapshotted at glLightfv under the view matrix). */
static void composeLighting(float *outAmbient, float *outLightPosEye,
                            float *outDiff, float *outSpec, float *outIsDir) {
	float amb[3] = {0.f, 0.f, 0.f};
	int primary = -1;
	for (int i = 0; i < PS3_NUM_LIGHTS; i++) {
		if (!g_light[i].enabled) continue;
		amb[0] += g_light[i].ambient[0];
		amb[1] += g_light[i].ambient[1];
		amb[2] += g_light[i].ambient[2];
		if (primary < 0) primary = i;
	}
	for (int i = 0; i < 3; i++) if (amb[i] > 1.f) amb[i] = 1.f;
	outAmbient[0] = amb[0]; outAmbient[1] = amb[1]; outAmbient[2] = amb[2];

	if (primary < 0) {
		outLightPosEye[0] = 0.f; outLightPosEye[1] = 1.f; outLightPosEye[2] = 0.f;
		outDiff[0] = outDiff[1] = outDiff[2] = 1.f;
		outSpec[0] = outSpec[1] = outSpec[2] = 0.f;
		*outIsDir = 1.f;
		return;
	}
	const Light &L = g_light[primary];
	outLightPosEye[0] = L.posEye[0];
	outLightPosEye[1] = L.posEye[1];
	outLightPosEye[2] = L.posEye[2];
	*outIsDir = L.isDir ? 1.f : 0.f;
	outDiff[0] = L.diffuse[0]; outDiff[1] = L.diffuse[1]; outDiff[2] = L.diffuse[2];
	outSpec[0] = L.specular[0]; outSpec[1] = L.specular[1]; outSpec[2] = L.specular[2];
}

static void waitVtxLabel(u32 val) {
	if (val == 0 || !g_vtxLabel) return;
	while ((s32)(*g_vtxLabel - val) < 0)
		usleep(10);
}

extern "C" void ps3_gl_flush(void) {
	if (g_immCount == 0 || !g_rsxReady) return;

	int n = g_immCount;
	if (n > PS3_IMM_MAX) n = PS3_IMM_MAX;

	u32 drawOffset;
	ImmVtx *drawBuf;
	int ringSlot = -1;

	if (n <= PS3_VTX_SLOT_MAX) {
		ringSlot = g_vtxRingHead;
		VtxSlot &slot = g_vtxRing[ringSlot];
		waitVtxLabel(slot.labelVal);
		drawBuf    = slot.buf;
		drawOffset = slot.offset;
		g_vtxRingHead = (g_vtxRingHead + 1) % PS3_VTX_RING;
	} else {
		u32 idleVal = g_vtxLabelNext++;
		if (g_vtxLabelNext == 0) g_vtxLabelNext = 1;
		rsxSetWriteBackendLabel(context, PS3_VTX_LABEL_IDX, idleVal);
		rsxFlushBuffer(context);
		waitVtxLabel(idleVal);
		drawBuf    = g_vtxOversize;
		drawOffset = g_vtxOversizeOff;
	}

	memcpy(drawBuf, g_immVtx, sizeof(ImmVtx) * n);

	setDrawEnv();
	bindTextureForDraw();

	/* POS / NRM / TEX0 / COL attribs */
	const u32 stride = sizeof(ImmVtx);
	rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
	                         drawOffset + 0,
	                         stride, 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_NORMAL, 0,
	                         drawOffset + sizeof(float) * 3,
	                         stride, 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0,
	                         drawOffset + sizeof(float) * 6,
	                         stride, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
	                         drawOffset + sizeof(float) * 8,
	                         stride, 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

	/* vertex program + uniforms (transposed matrices) */
	rsxLoadVertexProgram(context, g_vpo, g_vpUcode);
	float tmp[16];
	if (g_uProj) { matTranspose(tmp, g_proj.m); rsxSetVertexProgramParameter(context, g_vpo, g_uProj, tmp); }
	if (g_uMV)   { matTranspose(tmp, g_mv.m);   rsxSetVertexProgramParameter(context, g_vpo, g_uMV, tmp); }

	float doTexGen = (g_texGenS || g_texGenT) ? 1.f : 0.f;
	if (g_uDoTexGen)  rsxSetVertexProgramParameter(context, g_vpo, g_uDoTexGen, &doTexGen);
	if (g_uTexPlaneS) rsxSetVertexProgramParameter(context, g_vpo, g_uTexPlaneS, g_texPlaneS);
	if (g_uTexPlaneT) rsxSetVertexProgramParameter(context, g_vpo, g_uTexPlaneT, g_texPlaneT);

	/* fragment uniforms — set before Load so ucode embeds the new values */
	float ambient[3], lightPos[3], lightDiff[3], lightSpec[3], isDir;
	composeLighting(ambient, lightPos, lightDiff, lightSpec, &isDir);

	float anyLight = (g_light[0].enabled || g_light[1].enabled ||
	                  g_light[2].enabled || g_light[3].enabled) ? 1.f : 0.f;
	float doLighting = (g_lighting && anyLight) ? 1.f : 0.f;

	float doFog = g_fog ? 1.f : 0.f;
	float fogSE[2] = { g_fogStart, g_fogEnd };
	float fogCol[3] = { g_fogColor[0], g_fogColor[1], g_fogColor[2] };
	float matDiff[3] = { g_matDiffuse[0], g_matDiffuse[1], g_matDiffuse[2] };
	float matSpec[3] = { g_matSpecular[0], g_matSpecular[1], g_matSpecular[2] };
	float shin = g_matShininess > 0.f ? g_matShininess : 1.f;
	/* shader discard only for non-GEQUAL alpha tests (hw path covers GEQUAL) */
	float doATest = (g_alphaTest && g_alphaFunc != GL_GEQUAL) ? 1.f : 0.f;
	float aRef = g_alphaRef;

	if (g_uGlobalAmbient) rsxSetFragmentProgramParameter(context, g_fpo, g_uGlobalAmbient, ambient, g_fpOffset, GCM_LOCATION_RSX);
	if (g_uLightPos)      rsxSetFragmentProgramParameter(context, g_fpo, g_uLightPos, lightPos, g_fpOffset, GCM_LOCATION_RSX);
	if (g_uLightColor)    rsxSetFragmentProgramParameter(context, g_fpo, g_uLightColor, lightDiff, g_fpOffset, GCM_LOCATION_RSX);
	if (g_uLightSpec)     rsxSetFragmentProgramParameter(context, g_fpo, g_uLightSpec, lightSpec, g_fpOffset, GCM_LOCATION_RSX);
	if (g_uLightIsDir)    rsxSetFragmentProgramParameter(context, g_fpo, g_uLightIsDir, &isDir, g_fpOffset, GCM_LOCATION_RSX);
	if (g_uMatDiffuse)    rsxSetFragmentProgramParameter(context, g_fpo, g_uMatDiffuse, matDiff, g_fpOffset, GCM_LOCATION_RSX);
	if (g_uMatSpecular)   rsxSetFragmentProgramParameter(context, g_fpo, g_uMatSpecular, matSpec, g_fpOffset, GCM_LOCATION_RSX);
	if (g_uShininess)     rsxSetFragmentProgramParameter(context, g_fpo, g_uShininess, &shin, g_fpOffset, GCM_LOCATION_RSX);
	if (g_uDoLighting)    rsxSetFragmentProgramParameter(context, g_fpo, g_uDoLighting, &doLighting, g_fpOffset, GCM_LOCATION_RSX);
	if (g_uFogColor)      rsxSetFragmentProgramParameter(context, g_fpo, g_uFogColor, fogCol, g_fpOffset, GCM_LOCATION_RSX);
	if (g_uFogSE)         rsxSetFragmentProgramParameter(context, g_fpo, g_uFogSE, fogSE, g_fpOffset, GCM_LOCATION_RSX);
	if (g_uDoFog)         rsxSetFragmentProgramParameter(context, g_fpo, g_uDoFog, &doFog, g_fpOffset, GCM_LOCATION_RSX);
	if (g_uAlphaRef)      rsxSetFragmentProgramParameter(context, g_fpo, g_uAlphaRef, &aRef, g_fpOffset, GCM_LOCATION_RSX);
	if (g_uDoAlphaTest)   rsxSetFragmentProgramParameter(context, g_fpo, g_uDoAlphaTest, &doATest, g_fpOffset, GCM_LOCATION_RSX);

	rsxLoadFragmentProgramLocation(context, g_fpo, g_fpOffset, GCM_LOCATION_RSX);

	rsxSetUserClipPlaneControl(context,
		GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
		GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);

	rsxInvalidateVertexCache(context);
	rsxDrawVertexArray(context, mapPrim(g_primMode), 0, n);

	u32 doneVal = g_vtxLabelNext++;
	if (g_vtxLabelNext == 0) g_vtxLabelNext = 1;
	rsxSetWriteBackendLabel(context, PS3_VTX_LABEL_IDX, doneVal);
	rsxFlushBuffer(context);
	if (ringSlot >= 0)
		g_vtxRing[ringSlot].labelVal = doneVal;
}

/* =====================================================================
 * GLU
 * ===================================================================== */
void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble znear, GLdouble zfar) {
	GLdouble fh = tan(fovy * 3.14159265358979323846 / 360.0) * znear;
	GLdouble fw = fh * aspect;
	glFrustum(-fw, fw, -fh, fh, znear, zfar);
}

const GLubyte *gluErrorString(GLenum) {
	return (const GLubyte *)"PS3 GL shim: no error";
}

/* gluSphere: expand a unit sphere into triangle strips via immediate mode,
 * consuming the currently-bound material / color / lighting state. Used by
 * tux's body parts (DrawCharSphere). */
GLUquadricObj *gluNewQuadric(void)                 { return (GLUquadricObj *)1; }
void gluDeleteQuadric(GLUquadricObj*)              { }
void gluQuadricDrawStyle(GLUquadricObj*, GLenum)   { }
void gluQuadricOrientation(GLUquadricObj*, GLenum) { }
void gluQuadricNormals(GLUquadricObj*, GLenum)     { }

void gluSphere(GLUquadricObj*, GLdouble radius, GLint slices, GLint stacks) {
	if (slices < 3) slices = 3;
	if (stacks < 2) stacks = 2;
	/* Cap tessellation so a single sphere fits the imm buffer. */
	if (slices > 24) slices = 24;
	if (stacks > 16) stacks = 16;

	const float R = (float)radius;
	const float dTheta = 2.f * 3.14159265f / (float)slices;
	const float dPhi   = 3.14159265f / (float)stacks;

	for (int i = 0; i < stacks; i++) {
		float phi0 = (float)i * dPhi;
		float phi1 = (float)(i + 1) * dPhi;
		float y0 = cosf(phi0), r0 = sinf(phi0);
		float y1 = cosf(phi1), r1 = sinf(phi1);

		glBegin(GL_TRIANGLE_STRIP);
		for (int j = 0; j <= slices; j++) {
			float th = (float)j * dTheta;
			float ct = cosf(th), st = sinf(th);

			/* bottom of strip */
			float nx1 = r1 * ct, ny1 = y1, nz1 = r1 * st;
			glNormal3d(nx1, ny1, nz1);
			glTexCoord2f((float)j / (float)slices, (float)(i + 1) / (float)stacks);
			glVertex3d(R * nx1, R * ny1, R * nz1);

			/* top of strip */
			float nx0 = r0 * ct, ny0 = y0, nz0 = r0 * st;
			glNormal3d(nx0, ny0, nz0);
			glTexCoord2f((float)j / (float)slices, (float)i / (float)stacks);
			glVertex3d(R * nx0, R * ny0, R * nz0);
		}
		glEnd();
	}
}
