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
#include <altivec.h>

#include "rsxutil.h"
#include "ps3_gl_internal.h"
#include "ps3_tty.h"
#include "ps3_gfx_assert.h"
#include "ps3_log.h"
#include <stddef.h>

/* ---- embedded shader objects (bin2o from etr3d.vcg/.fcg) ---- */
extern "C" const u8 etr3d_vpo[];
extern "C" const u8 etr3d_fpo[];
extern "C" const u8 etr_terrain_vpo[];
extern "C" const u8 etr_terrain_fpo[];
extern "C" const u8 etr_ui_fpo[];

extern "C" void ps3_gl_flush(void);
static void waitVtxLabel(u32 val);

/* =====================================================================
 * Internal state
 * ===================================================================== */

#define PS3_MATRIX_STACK_DEPTH 32
#define PS3_ATTRIB_STACK_DEPTH 16
#define PS3_IMM_MAX            4096
#define PS3_NUM_LIGHTS         4

struct MatrixStack {
	alignas(16) float m[16];
	float stack[PS3_MATRIX_STACK_DEPTH][16];
	int   top;
};
static_assert(alignof(MatrixStack) >= 16,
              "MatrixStack must be 16-byte aligned for VMX loads");

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
/* Compile-time guarantee that the GCM attrib offsets used in ps3_gl_flush
 * still match this struct if anyone reshuffles the fields. Mismatched
 * stride/offset is the classic cause of "blown up triangles" on real
 * RSX — RPCS3 tends to render fine because it re-validates per attrib. */
static_assert(sizeof(ImmVtx) == 12 * sizeof(float),
              "ImmVtx must be 12 packed floats (POS+NRM+TEX0+COL)");
static_assert(offsetof(ImmVtx, x)  == 0,                  "POS at offset 0");
static_assert(offsetof(ImmVtx, nx) == 3 * sizeof(float),  "NORMAL at offset 12");
static_assert(offsetof(ImmVtx, u)  == 6 * sizeof(float),  "TEX0 at offset 24");
static_assert(offsetof(ImmVtx, r)  == 8 * sizeof(float),  "COLOR0 at offset 32");
static ImmVtx  g_immVtx[PS3_IMM_MAX];
static int     g_immCount = 0;
static GLenum  g_primMode = GL_TRIANGLES;
static int     g_inBegin  = 0;
static int     g_beginStartCount = 0;
static GLboolean g_batchPending = GL_FALSE;
static float   g_curU = 0.f, g_curV = 0.f;
static float   g_curNx = 0.f, g_curNy = 1.f, g_curNz = 0.f;

/* Per-flush dirty bits. State mutators set the bit(s) for the categories
 * they touch; ps3_gl_flush() skips the corresponding rsx command block
 * when clean and clears the mask at the end of the flush. The RSX keeps
 * the last-emitted value for any state we don't re-send, so skipping is
 * equivalent to "state hasn't changed, don't bother re-emitting". */
enum DirtyBit {
	DIRTY_VIEWPORT     = 1 << 0,
	DIRTY_DEPTH        = 1 << 1,
	DIRTY_CULL         = 1 << 2,
	DIRTY_BLEND        = 1 << 3,
	DIRTY_ALPHA        = 1 << 4,
	DIRTY_FIXED_ENV    = 1 << 5,
	DIRTY_TEXTURE      = 1 << 6,
	DIRTY_PROJ_MATRIX  = 1 << 7,
	DIRTY_MV_MATRIX    = 1 << 8,
	DIRTY_TEXGEN       = 1 << 9,
	DIRTY_VP_LIGHTING  = 1 << 10,
	DIRTY_DRAW_ENV     = DIRTY_VIEWPORT | DIRTY_DEPTH | DIRTY_CULL |
	                     DIRTY_BLEND | DIRTY_ALPHA | DIRTY_FIXED_ENV,
	DIRTY_ALL         = ~0u,
};
static u32 g_dirtyBits = DIRTY_ALL;

enum VertexProgramKind {
	VP_NONE,
	VP_GENERAL,
	VP_TERRAIN,
};
static VertexProgramKind g_loadedVertexProgram = VP_NONE;

/* Route a matrix mutation to the bit for the currently-active stack.
 * Mirrors activeMatrix(): only PROJECTION and MODELVIEW are uploaded
 * (texture/color matrices aren't read by the shader). */
static inline u32 matrixDirtyBit() {
	return (g_matrixMode == GL_PROJECTION) ? DIRTY_PROJ_MATRIX : DIRTY_MV_MATRIX;
}

/* Vertex draw ring (fencing prevents CPU overwrite of in-flight draws).
 * Label 253; rsxutil uses 255 for flip/idle. */
/* A race frame submits roughly 115–120 small legacy-GL batches. Keep two
 * complete frames of staging slots so the PPU can feed RSX continuously
 * instead of blocking every 32 draws and leaving bubbles in the command
 * stream. Double-buffered presentation bounds the actual in-flight usage. */
#define PS3_VTX_RING      256
#define PS3_VTX_SLOT_MAX  512
#define PS3_OVERSIZE_RING 16
#define PS3_VTX_LABEL_IDX 253
#define PS3_SUBMIT_BATCH_DRAWS 16
struct VtxSlot {
	ImmVtx *buf;
	u32     offset;
	u32     labelVal;
};
static VtxSlot  g_vtxRing[PS3_VTX_RING];
static int      g_vtxRingHead = 0;
static VtxSlot  g_vtxOversizeRing[PS3_OVERSIZE_RING];
static int      g_vtxOversizeRingHead = 0;
static vu32    *g_vtxLabel       = NULL;
static u32      g_vtxLabelNext   = 1;
static u32      g_unflushedDraws = 0;

/* Array draws can decode directly into an acquired RSX ring slot. The next
 * ps3_gl_flush consumes this one-shot reservation instead of acquiring a
 * second slot and memcpying g_immVtx into it. */
static ImmVtx  *g_preparedDrawBuf = NULL;
static u32      g_preparedDrawOffset = 0;
static int      g_preparedRingSlot = -1;
static int      g_preparedOversizeRingSlot = -1;

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
static u32                 g_fpUcodeSize = 0;

/* The UI program is immutable (sampler * vertex colour), so unlike the 3D
 * program it needs only one RSX-local copy and no per-draw constant patching. */
static rsxFragmentProgram *g_uiFpo = NULL;
static void               *g_uiFpUcode = NULL;
static u32                *g_uiFpBuf = NULL;
static u32                 g_uiFpOffset = 0;
static rsxProgramAttrib   *g_uiTexSampler = NULL;

/* Fragment-program ring.
 *
 * rsxSetFragmentProgramParameter patches constants INTO the RSX-local
 * programubuffer via InlineTransfer. The previous single-buffer layout let
 * draw N+1 rewrite uniforms while draw N was still executing from that same
 * buffer — torn floats for doLighting / fog / materials surface as red-tinted
 * menus, white sparkles on HUD digits, and scrambled textures after the 3D
 * port punted ~14 uniforms every flush (vs. the old 2D path's one uColor).
 *
 * Ring slots are acquired under the same backend label fence as the vertex
 * ring so a slot is never rewritten until the RSX has finished the draw that
 * last used it. */
struct FpSlot {
	u32 *buf;
	u32  offset;
	u32  labelVal;
	u32  generation;
};
static FpSlot g_fpRing[PS3_VTX_RING];
static int g_fpRingHead = 0;
static FpSlot *g_currentFpSlot = NULL;
static u32 g_fullFpGeneration = 1;

/* Terrain uses a no-specular fragment variant.  It is ringed independently
 * because its constants are patched per draw just like the general 3D path. */
static rsxFragmentProgram *g_terrainFpo = NULL;
static void               *g_terrainFpUcode = NULL;
static u32                 g_terrainFpUcodeSize = 0;
static FpSlot              g_terrainFpRing[PS3_VTX_RING];
static int                 g_terrainFpRingHead = 0;
static FpSlot             *g_currentTerrainFpSlot = NULL;
static u32                 g_liteFpGeneration = 1;
static rsxProgramConst     *g_tFogColor = NULL;
static rsxProgramConst     *g_tFogSE = NULL;
static rsxProgramConst     *g_tDoFog = NULL;
static rsxProgramConst     *g_tOutputScale = NULL;
static rsxProgramAttrib    *g_tTexSampler = NULL;

static rsxProgramConst *g_uProj = NULL;
static rsxProgramConst *g_uMV = NULL;
static rsxProgramConst *g_uTexPlaneS = NULL;
static rsxProgramConst *g_uTexPlaneT = NULL;
static rsxProgramConst *g_uDoTexGen = NULL;

static rsxVertexProgram *g_terrainVpo = NULL;
static void             *g_terrainVpUcode = NULL;
static rsxProgramConst  *g_tvProj = NULL;
static rsxProgramConst  *g_tvMV = NULL;
static rsxProgramConst  *g_tvTexPlaneS = NULL;
static rsxProgramConst  *g_tvTexPlaneT = NULL;
static rsxProgramConst  *g_tvDoTexGen = NULL;
static rsxProgramConst  *g_tvGlobalAmbient = NULL;
static rsxProgramConst  *g_tvLightPos = NULL;
static rsxProgramConst  *g_tvLightColor = NULL;
static rsxProgramConst  *g_tvLightIsDir = NULL;
static rsxProgramConst  *g_tvMatDiffuse = NULL;

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
static rsxProgramConst *g_uOutputScale = NULL;
static rsxProgramAttrib *g_texSampler = NULL;

enum FragmentProgramKind {
	FP_NONE,
	FP_UI,
	FP_LITE,
	FP_FULL,
};
static FragmentProgramKind g_loadedFragmentProgram = FP_NONE;
static u32 g_loadedFragmentOffset = 0;

static inline void bumpGeneration(u32 &generation) {
	if (++generation == 0) generation = 1;
}

static inline void dirtyFullFragmentProgram(void) {
	bumpGeneration(g_fullFpGeneration);
}

static inline void dirtyFogFragmentPrograms(void) {
	bumpGeneration(g_fullFpGeneration);
	bumpGeneration(g_liteFpGeneration);
}

/* white 1x1 fallback */
static u32 *g_whiteBuf = NULL;
static u32  g_whiteOffset = 0;

static GLboolean useUiFragmentProgram(void) {
	/* This is the old 2D workload: alpha-blended screen geometry with depth,
	 * lighting and alpha-test disabled. Fog can remain set after the 3D pass;
	 * it must not tint HUD/menu transparent texels. */
	return g_blend && !g_depthTest && !g_lighting && !g_alphaTest;
}

static GLboolean useTerrainFragmentProgram(void) {
	/* Physical RSX cannot sustain the full per-pixel normalize + pow()
	 * lighting path for Tux's many overlapping spheres at 1080p. Use the
	 * per-vertex diffuse path for all lit geometry; trees retain hardware
	 * GEQUAL alpha testing. This intentionally drops subtle specular
	 * highlights in exchange for consistent full-rate rendering. */
	return g_lighting &&
	       (!g_alphaTest || g_alphaFunc == GL_GEQUAL);
}

static GLboolean useLiteFragmentProgram(void) {
	if (useTerrainFragmentProgram()) return GL_TRUE;
	/* The terrain fragment stage is also exactly the unlit fixed-function
	 * equation (texture * vertex colour + fog).  Keep non-GEQUAL alpha tests
	 * on etr3d because they require its shader discard path. */
	return !useUiFragmentProgram() &&
	       !g_lighting &&
	       (!g_alphaTest || g_alphaFunc == GL_GEQUAL);
}

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

static void flushPendingImmediate(void) {
	if (!g_batchPending || g_immCount == 0 || g_inBegin) return;
	ps3_gl_flush();
	g_immCount = 0;
	g_batchPending = GL_FALSE;
}

extern "C" void ps3_gl_flush_pending(void) {
	flushPendingImmediate();
}

extern "C" void ps3_gl_invalidate_rsx_state(void) {
	/* flip()/setRenderTarget() is outside this shim and can invalidate the
	 * assumptions behind its per-category state cache. Force one complete
	 * state/program/texture rebind on the first draw of the new surface. */
	g_dirtyBits = DIRTY_ALL;
	g_loadedVertexProgram = VP_NONE;
	g_loadedFragmentProgram = FP_NONE;
	g_loadedFragmentOffset = 0;
}

/* =====================================================================
 * Matrix math (column-major)
 *
 * VMX-accelerated 4x4 multiply / transpose. Column-major storage means
 * a column of A is one contiguous vector float — exactly the input shape
 * vec_madd wants. Inputs must be 16-byte aligned (MatrixStack::m carries
 * alignas(16); every local float[16] matrix in this file is annotated the
 * same way so the same vec_ld/vec_st path is safe for callers like
 * glTranslatef/glRotatef that pass a stack-local temporary).
 * ===================================================================== */
static inline vector float vmxLoadMatCol(const float *m, int col) {
	return vec_ld(col * 16, m);
}

static void matIdentity(float *m) {
	memset(m, 0, sizeof(float) * 16);
	m[0] = m[5] = m[10] = m[15] = 1.f;
}

static void matMul(float *out, const float *a, const float *b) {
	/* out[col][row] = sum_k a[k][row] * b[col][k]
	 * = sum_k a_col_k * b[col][k]
	 * Load A's four columns once; each output column is then four splat/
	 * madd operations weighted by the corresponding element of B's column. */
	alignas(16) float t[16];
	const vector float a0 = vmxLoadMatCol(a, 0);
	const vector float a1 = vmxLoadMatCol(a, 1);
	const vector float a2 = vmxLoadMatCol(a, 2);
	const vector float a3 = vmxLoadMatCol(a, 3);
	const vector float zero = (vector float){0.f, 0.f, 0.f, 0.f};

	for (int col = 0; col < 4; col++) {
		const vector float bc = vmxLoadMatCol(b, col);
		vector float r = vec_madd(a0, vec_splat(bc, 0), zero);
		r = vec_madd(a1, vec_splat(bc, 1), r);
		r = vec_madd(a2, vec_splat(bc, 2), r);
		r = vec_madd(a3, vec_splat(bc, 3), r);
		vec_st(r, col * 16, t);
	}
	memcpy(out, t, sizeof(float) * 16);
}

static void matTranspose(float *out, const float *m) {
	/* Canonical AltiVec 4x4 transpose: load four column vectors, two
	 * mergeh/mergel pairs build two intermediate shuffles, two more
	 * produce the four output rows. out never aliases m here (the only
	 * caller passes &tmp in ps3_gl_flush), and every input is consumed
	 * into registers before any store issues, so aliasing is moot.
	 * Inline the final merge into vec_st — GCC's -Wunused-but-set-variable
	 * doesn't track vector intrinsic argument uses cleanly. */
	const vector float c0 = vmxLoadMatCol(m, 0);
	const vector float c1 = vmxLoadMatCol(m, 1);
	const vector float c2 = vmxLoadMatCol(m, 2);
	const vector float c3 = vmxLoadMatCol(m, 3);

	const vector float t0 = vec_mergeh(c0, c2);
	const vector float t1 = vec_mergel(c0, c2);
	const vector float t2 = vec_mergeh(c1, c3);
	const vector float t3 = vec_mergel(c1, c3);

	vec_st(vec_mergeh(t0, t2), 0,  out);
	vec_st(vec_mergel(t0, t2), 16, out);
	vec_st(vec_mergeh(t1, t3), 32, out);
	vec_st(vec_mergel(t1, t3), 48, out);
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
	flushPendingImmediate();
	MatrixStack *s = activeMatrix();
	if (s->top > 0) {
		s->top--;
		memcpy(s->m, s->stack[s->top], sizeof(float) * 16);
	}
	g_dirtyBits |= matrixDirtyBit();
}

void glLoadIdentity(void) {
	flushPendingImmediate();
	matIdentity(activeMatrix()->m);
	g_dirtyBits |= matrixDirtyBit();
}

void glLoadMatrixd(const GLdouble *m) {
	flushPendingImmediate();
	for (int i = 0; i < 16; i++) activeMatrix()->m[i] = (float)m[i];
	g_dirtyBits |= matrixDirtyBit();
}

void glMultMatrixd(const GLdouble *m) {
	flushPendingImmediate();
	alignas(16) float mf[16];
	for (int i = 0; i < 16; i++) mf[i] = (float)m[i];
	MatrixStack *s = activeMatrix();
	alignas(16) float tmp[16];
	matMul(tmp, s->m, mf);
	memcpy(s->m, tmp, sizeof(float) * 16);
	g_dirtyBits |= matrixDirtyBit();
}

void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
	flushPendingImmediate();
	alignas(16) float m[16];
	matIdentity(m);
	m[0]  = 2.f / (float)(r - l);
	m[5]  = 2.f / (float)(t - b);
	m[10] = -2.f / (float)(f - n);
	m[12] = -(float)(r + l) / (float)(r - l);
	m[13] = -(float)(t + b) / (float)(t - b);
	m[14] = -(float)(f + n) / (float)(f - n);
	MatrixStack *s = activeMatrix();
	alignas(16) float tmp[16];
	matMul(tmp, s->m, m);
	memcpy(s->m, tmp, sizeof(float) * 16);
	g_dirtyBits |= matrixDirtyBit();
}

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
	flushPendingImmediate();
	alignas(16) float m[16];
	memset(m, 0, sizeof(float) * 16);
	m[0]  = (float)(2.0 * n / (r - l));
	m[5]  = (float)(2.0 * n / (t - b));
	m[8]  = (float)((r + l) / (r - l));
	m[9]  = (float)((t + b) / (t - b));
	m[10] = (float)(-(f + n) / (f - n));
	m[11] = -1.f;
	m[14] = (float)(-2.0 * f * n / (f - n));
	MatrixStack *s = activeMatrix();
	alignas(16) float tmp[16];
	matMul(tmp, s->m, m);
	memcpy(s->m, tmp, sizeof(float) * 16);
	g_dirtyBits |= matrixDirtyBit();
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
	flushPendingImmediate();
	alignas(16) float m[16];
	matIdentity(m);
	m[12] = x; m[13] = y; m[14] = z;
	MatrixStack *s = activeMatrix();
	alignas(16) float tmp[16];
	matMul(tmp, s->m, m);
	memcpy(s->m, tmp, sizeof(float) * 16);
	g_dirtyBits |= matrixDirtyBit();
}
void glTranslated(GLdouble x, GLdouble y, GLdouble z) {
	glTranslatef((GLfloat)x, (GLfloat)y, (GLfloat)z);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
	flushPendingImmediate();
	float c = cosf(angle * (3.14159265f / 180.f));
	float s = sinf(angle * (3.14159265f / 180.f));
	float len = sqrtf(x * x + y * y + z * z);
	if (len == 0.f) return;
	x /= len; y /= len; z /= len;
	alignas(16) float m[16];
	memset(m, 0, sizeof(float) * 16);
	m[0]  = x*x*(1-c)+c;   m[4]  = x*y*(1-c)-z*s; m[8]  = x*z*(1-c)+y*s;
	m[1]  = y*x*(1-c)+z*s; m[5]  = y*y*(1-c)+c;   m[9]  = y*z*(1-c)-x*s;
	m[2]  = x*z*(1-c)-y*s; m[6]  = y*z*(1-c)+x*s; m[10] = z*z*(1-c)+c;
	m[15] = 1.f;
	MatrixStack *stk = activeMatrix();
	alignas(16) float tmp[16];
	matMul(tmp, stk->m, m);
	memcpy(stk->m, tmp, sizeof(float) * 16);
	g_dirtyBits |= matrixDirtyBit();
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
	if (g_viewport[0] == x && g_viewport[1] == y &&
	    g_viewport[2] == w && g_viewport[3] == h) return;
	flushPendingImmediate();
	g_viewport[0] = x; g_viewport[1] = y; g_viewport[2] = w; g_viewport[3] = h;
	g_dirtyBits |= DIRTY_VIEWPORT;
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
	flushPendingImmediate();
	if (g_attribTop > 0)
		restoreState(&g_attrib[--g_attribTop]);
	/* restoreState touches draw-env caps, current texture, and texgen
	 * enables. Conservatively re-emit all of those on the next flush. */
	g_dirtyBits |= DIRTY_DEPTH | DIRTY_CULL | DIRTY_BLEND | DIRTY_ALPHA |
	               DIRTY_TEXTURE | DIRTY_TEXGEN | DIRTY_VP_LIGHTING;
	dirtyFogFragmentPrograms();
}

static int lightIndex(GLenum cap) {
	if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + PS3_NUM_LIGHTS)
		return (int)(cap - GL_LIGHT0);
	return -1;
}

void glEnable(GLenum cap) {
	int li;
	switch (cap) {
		case GL_TEXTURE_2D:
			if (!g_tex2d) { flushPendingImmediate(); g_tex2d = GL_TRUE; g_dirtyBits |= DIRTY_TEXTURE; }
			break;
		case GL_BLEND:
			if (!g_blend) { flushPendingImmediate(); g_blend = GL_TRUE; g_dirtyBits |= DIRTY_BLEND; }
			break;
		case GL_DEPTH_TEST:
			if (!g_depthTest) { flushPendingImmediate(); g_depthTest = GL_TRUE; g_dirtyBits |= DIRTY_DEPTH | DIRTY_BLEND; }
			break;
		case GL_CULL_FACE:
			if (!g_cullFace) { flushPendingImmediate(); g_cullFace = GL_TRUE; g_dirtyBits |= DIRTY_CULL | DIRTY_BLEND; }
			break;
		case GL_LIGHTING:
			if (!g_lighting) { flushPendingImmediate(); g_lighting = GL_TRUE; g_dirtyBits |= DIRTY_VP_LIGHTING | DIRTY_BLEND; dirtyFullFragmentProgram(); }
			break;
		case GL_ALPHA_TEST:
			if (!g_alphaTest) { flushPendingImmediate(); g_alphaTest = GL_TRUE; g_dirtyBits |= DIRTY_ALPHA | DIRTY_BLEND; dirtyFullFragmentProgram(); }
			break;
		case GL_STENCIL_TEST:
			g_stencilTest = GL_TRUE; /* not emitted by setDrawEnv */
			break;
		case GL_FOG:
			if (!g_fog) { flushPendingImmediate(); g_fog = GL_TRUE; dirtyFogFragmentPrograms(); }
			break;
		case GL_TEXTURE_GEN_S:
			if (!g_texGenS) { flushPendingImmediate(); g_texGenS = GL_TRUE; g_dirtyBits |= DIRTY_TEXGEN | DIRTY_BLEND; }
			break;
		case GL_TEXTURE_GEN_T:
			if (!g_texGenT) { flushPendingImmediate(); g_texGenT = GL_TRUE; g_dirtyBits |= DIRTY_TEXGEN | DIRTY_BLEND; }
			break;
		case GL_NORMALIZE:      g_normalize = GL_TRUE; break;  /* not uploaded */
		case GL_COLOR_MATERIAL: break; /* material always tracks glColor lightpath via vColor */
		default:
			if ((li = lightIndex(cap)) >= 0 && !g_light[li].enabled) {
				flushPendingImmediate();
				g_light[li].enabled = GL_TRUE;
				g_dirtyBits |= DIRTY_VP_LIGHTING;
				dirtyFullFragmentProgram();
			}
			break;
	}
}

void glDisable(GLenum cap) {
	int li;
	switch (cap) {
		case GL_TEXTURE_2D:
			if (g_tex2d) { flushPendingImmediate(); g_tex2d = GL_FALSE; g_dirtyBits |= DIRTY_TEXTURE; }
			break;
		case GL_BLEND:
			if (g_blend) { flushPendingImmediate(); g_blend = GL_FALSE; g_dirtyBits |= DIRTY_BLEND; }
			break;
		case GL_DEPTH_TEST:
			if (g_depthTest) { flushPendingImmediate(); g_depthTest = GL_FALSE; g_dirtyBits |= DIRTY_DEPTH | DIRTY_BLEND; }
			break;
		case GL_CULL_FACE:
			if (g_cullFace) { flushPendingImmediate(); g_cullFace = GL_FALSE; g_dirtyBits |= DIRTY_CULL | DIRTY_BLEND; }
			break;
		case GL_LIGHTING:
			if (g_lighting) { flushPendingImmediate(); g_lighting = GL_FALSE; g_dirtyBits |= DIRTY_VP_LIGHTING | DIRTY_BLEND; dirtyFullFragmentProgram(); }
			break;
		case GL_ALPHA_TEST:
			if (g_alphaTest) { flushPendingImmediate(); g_alphaTest = GL_FALSE; g_dirtyBits |= DIRTY_ALPHA | DIRTY_BLEND; dirtyFullFragmentProgram(); }
			break;
		case GL_STENCIL_TEST:   g_stencilTest = GL_FALSE; break;
		case GL_FOG:
			if (g_fog) { flushPendingImmediate(); g_fog = GL_FALSE; dirtyFogFragmentPrograms(); }
			break;
		case GL_TEXTURE_GEN_S:
			if (g_texGenS) { flushPendingImmediate(); g_texGenS = GL_FALSE; g_dirtyBits |= DIRTY_TEXGEN | DIRTY_BLEND; }
			break;
		case GL_TEXTURE_GEN_T:
			if (g_texGenT) { flushPendingImmediate(); g_texGenT = GL_FALSE; g_dirtyBits |= DIRTY_TEXGEN | DIRTY_BLEND; }
			break;
		case GL_NORMALIZE:      g_normalize = GL_FALSE; break;
		default:
			if ((li = lightIndex(cap)) >= 0 && g_light[li].enabled) {
				flushPendingImmediate();
				g_light[li].enabled = GL_FALSE;
				g_dirtyBits |= DIRTY_VP_LIGHTING;
				dirtyFullFragmentProgram();
			}
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

void glDepthMask(GLboolean f) {
	if (g_depthMask == f) return;
	flushPendingImmediate();
	g_depthMask = f;
	g_dirtyBits |= DIRTY_DEPTH | DIRTY_BLEND;
}
void glDepthFunc(GLenum f) {
	if (g_depthFunc == f) return;
	flushPendingImmediate();
	g_depthFunc = f;
	g_dirtyBits |= DIRTY_DEPTH;
}
void glShadeModel(GLenum)     { }
void glAlphaFunc(GLenum f, GLclampf r) {
	if (g_alphaFunc == f && g_alphaRef == r) return;
	flushPendingImmediate();
	g_alphaFunc = f; g_alphaRef = r;
	g_dirtyBits |= DIRTY_ALPHA | DIRTY_BLEND;
	dirtyFullFragmentProgram();
}
void glStencilFunc(GLenum, GLint, GLuint) { }
void glStencilOp(GLenum, GLenum, GLenum)  { }
void glBlendFunc(GLenum sf, GLenum df) {
	if (g_blendSrc == sf && g_blendDst == df) return;
	flushPendingImmediate();
	g_blendSrc = sf; g_blendDst = df;
	g_dirtyBits |= DIRTY_BLEND;
}

/* =====================================================================
 * GL: lighting / material / fog / texgen
 * ===================================================================== */
void glLightfv(GLenum light, GLenum pname, const GLfloat *p) {
	int li = lightIndex(light);
	if (li < 0 || !p) return;
	Light &L = g_light[li];
	Light next = L;
	switch (pname) {
		case GL_POSITION: {
			/* OpenGL freezes light position into eye-space under the CURRENT
			 * modelview (camera already applied when Env.SetupLight runs). */
			float eye[4];
			matMulVec4(eye, g_mv.m, p);
			if (p[3] == 0.f) {
				next.posEye[0] = eye[0]; next.posEye[1] = eye[1]; next.posEye[2] = eye[2];
				next.isDir = GL_TRUE;
			} else {
				float iw = (eye[3] != 0.f) ? (1.f / eye[3]) : 1.f;
				next.posEye[0] = eye[0] * iw;
				next.posEye[1] = eye[1] * iw;
				next.posEye[2] = eye[2] * iw;
				next.isDir = GL_FALSE;
			}
			break;
		}
		case GL_AMBIENT:  memcpy(next.ambient,  p, 4 * sizeof(float)); break;
		case GL_DIFFUSE:  memcpy(next.diffuse,  p, 4 * sizeof(float)); break;
		case GL_SPECULAR: memcpy(next.specular, p, 4 * sizeof(float)); break;
		default: return;
	}
	if (memcmp(&L, &next, sizeof(L)) == 0) return;
	flushPendingImmediate();
	L = next;
	g_dirtyBits |= DIRTY_VP_LIGHTING;
	dirtyFullFragmentProgram();
}

void glMaterialf(GLenum, GLenum pname, GLfloat v) {
	if (pname == GL_SHININESS && g_matShininess != v) {
		flushPendingImmediate();
		g_matShininess = v;
		g_dirtyBits |= DIRTY_VP_LIGHTING;
		dirtyFullFragmentProgram();
	}
}

void glMaterialfv(GLenum, GLenum pname, const GLfloat *p) {
	if (!p) return;
	float *destination = NULL;
	switch (pname) {
		case GL_AMBIENT_AND_DIFFUSE:
		case GL_DIFFUSE:  destination = g_matDiffuse; break;
		case GL_SPECULAR: destination = g_matSpecular; break;
		case GL_AMBIENT:  /* absorbed into light ambient; ignore material ambient */ return;
		default: return;
	}
	if (memcmp(destination, p, 4 * sizeof(float)) == 0) return;
	flushPendingImmediate();
	memcpy(destination, p, 4 * sizeof(float));
	g_dirtyBits |= DIRTY_VP_LIGHTING;
	dirtyFullFragmentProgram();
}

void glFogi(GLenum pname, GLint v) {
	(void)pname; (void)v; /* only GL_LINEAR is supported */
}
void glFogf(GLenum pname, GLfloat v) {
	float *destination = pname == GL_FOG_START ? &g_fogStart :
	                     pname == GL_FOG_END ? &g_fogEnd : NULL;
	if (!destination || *destination == v) return;
	flushPendingImmediate();
	*destination = v;
	dirtyFogFragmentPrograms();
}
void glFogfv(GLenum pname, const GLfloat *p) {
	if (pname == GL_FOG_COLOR && p) {
		if (memcmp(g_fogColor, p, 4 * sizeof(float)) == 0) return;
		flushPendingImmediate();
		memcpy(g_fogColor, p, 4 * sizeof(float));
		dirtyFogFragmentPrograms();
	}
}
void glHint(GLenum, GLenum) { }

void glTexGeni(GLenum, GLenum, GLint) { /* only OBJECT_LINEAR is used */ }
void glTexGenfv(GLenum coord, GLenum pname, const GLfloat *p) {
	if (pname != GL_OBJECT_PLANE || !p) return;
	float *destination = coord == GL_S ? g_texPlaneS :
	                     coord == GL_T ? g_texPlaneT : NULL;
	if (!destination || memcmp(destination, p, 4 * sizeof(float)) == 0) return;
	flushPendingImmediate();
	memcpy(destination, p, 4 * sizeof(float));
	g_dirtyBits |= DIRTY_TEXGEN;
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

static int batchablePrimitiveStep(GLenum mode) {
	switch (mode) {
		case GL_QUADS:     return 4;
		case GL_TRIANGLES: return 3;
		case GL_LINES:     return 2;
		case GL_POINTS:    return 1;
		default:           return 0;
	}
}

void glBegin(GLenum mode) {
	const int step = batchablePrimitiveStep(mode);
	if (g_batchPending && (mode != g_primMode || step == 0))
		flushPendingImmediate();
	if (!g_batchPending) {
		g_primMode = mode;
		g_immCount = 0;
	}
	g_beginStartCount = g_immCount;
	g_inBegin = 1;
}

void glEnd(void) {
	if (!g_inBegin) return;
	g_inBegin = 0;
	const int step = batchablePrimitiveStep(g_primMode);
	if (step > 0) {
		/* Incomplete primitives are discarded at each glBegin/glEnd boundary;
		 * otherwise their vertices could incorrectly join the next batch. */
		const int added = g_immCount - g_beginStartCount;
		g_immCount -= added % step;
		g_batchPending = (g_immCount > 0) ? GL_TRUE : GL_FALSE;
		return;
	}
	if (g_immCount > 0) ps3_gl_flush();
	g_immCount = 0;
	g_batchPending = GL_FALSE;
}

static void pushImm(float x, float y, float z) {
	const int step = batchablePrimitiveStep(g_primMode);
	const int limit = step ? (PS3_IMM_MAX / step) * step : PS3_IMM_MAX;
	if (step && g_immCount >= limit) {
		ps3_gl_flush();
		g_immCount = 0;
		g_beginStartCount = 0;
		g_batchPending = GL_FALSE;
	}
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

/* Decoded view of a client array, computed once per draw call.
 * Hoists the per-vertex pointer-NULL check, stride normalization
 * (stride==0 → tightly packed), and the type switch out of the inner
 * loop in the gather fast paths below. */
struct ArrayView {
	const u8 *base;
	GLsizei   stride;
	GLenum    type;
	GLint     size;
	bool      enabled;
};
static inline ArrayView decodeArray(const ClientArray &a) {
	ArrayView v;
	v.enabled = (a.enabled == GL_TRUE) && a.pointer != NULL;
	if (v.enabled) {
		v.base   = (const u8 *)a.pointer;
		v.stride = a.stride ? a.stride : (GLsizei)(sizeofGlType(a.type) * a.size);
		v.type   = a.type;
		v.size   = a.size;
	} else {
		v.base = NULL; v.stride = 0; v.type = GL_FLOAT; v.size = 0;
	}
	return v;
}

/* Indexed vertex gather for the common float3 position / float3 normal /
 * float2 texcoord / ubyte4 color layouts ETR uses for course geometry.
 * Hoisting the type dispatch out of the inner loop cuts per-vertex work
 * from ~8 readArrayComp calls (each doing NULL-check + stride compute +
 * type switch) down to one byte-stride pointer add per array.
 *
 * Template flags specialize at compile time so the inner loop has no
 * per-vertex branches for missing arrays; Idx selects the index type. */
template<bool HasNrm, bool HasColUbyte, typename Idx>
static inline void gatherIndexed(const ArrayView &pos, const ArrayView &nrm,
                                 const ArrayView &tex, const ArrayView &col,
                                 GLsizei count, const Idx *indices,
                                 ImmVtx *out) {
	static const float kInv255 = 1.f / 255.f;
	const u8   *const pB = pos.base;
	const u8   *const nB = nrm.base;
	const u8   *const tB = tex.base;
	const u8   *const cB = col.base;
	const GLsizei pS = pos.stride;
	const GLsizei nS = nrm.stride;
	const GLsizei tS = tex.stride;
	const GLsizei cS = col.stride;
	const GLint   cSz = col.size;
	const float cr = g_color[0], cg = g_color[1], cb = g_color[2], ca = g_color[3];
	const float cnx = g_curNx, cny = g_curNy, cnz = g_curNz;

	for (GLsizei i = 0; i < count; i++) {
		const GLuint idx = (GLuint)indices[i];
		ImmVtx &v = out[i];
		const float *p = (const float *)(pB + (u32)idx * (u32)pS);
		v.x = p[0]; v.y = p[1]; v.z = p[2];
		if (HasNrm) {
			const float *n = (const float *)(nB + (u32)idx * (u32)nS);
			v.nx = n[0]; v.ny = n[1]; v.nz = n[2];
		} else {
			v.nx = cnx; v.ny = cny; v.nz = cnz;
		}
		const float *t = (const float *)(tB + (u32)idx * (u32)tS);
		v.u = t[0]; v.v = t[1];
		if (HasColUbyte) {
			const u8 *c = cB + (u32)idx * (u32)cS;
			v.r = c[0] * kInv255; v.g = c[1] * kInv255;
			v.b = c[2] * kInv255;
			v.a = (cSz > 3) ? c[3] * kInv255 : 1.f;
		} else {
			v.r = cr; v.g = cg; v.b = cb; v.a = ca;
		}
	}
}

/* Pick the gather variant for the current client-array state. Returns
 * true and fills outHasNrm/outHasColUbyte on a fast-path match; false
 * means fall back to fetchVertex(). Only float position+texcoord (with
 * optional float normal and ubyte color) are fast-pathed — these are
 * the only layouts the game's course geometry uses. */
static bool pickGatherVariant(const ArrayView &pos, const ArrayView &nrm,
                              const ArrayView &tex, const ArrayView &col,
                              bool &outHasNrm, bool &outHasColUbyte) {
	const bool posF3 = pos.enabled && pos.type == GL_FLOAT && pos.size == 3;
	const bool texF2 = tex.enabled && tex.type == GL_FLOAT && tex.size == 2;
	if (!posF3 || !texF2) return false;
	const bool nrmF3 = nrm.enabled && nrm.type == GL_FLOAT && nrm.size == 3;
	const bool colUB = col.enabled && col.type == GL_UNSIGNED_BYTE;
	if (nrmF3) {
		if (colUB) { outHasNrm = true;  outHasColUbyte = true;  return true; }
		if (!col.enabled) {
			outHasNrm = true;  outHasColUbyte = false; return true;
		}
		return false;
	}
	if (!nrm.enabled && !col.enabled) {
		outHasNrm = false; outHasColUbyte = false; return true;
	}
	return false;
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

static ImmVtx *prepareDirectArrayDraw(GLsizei count) {
	GFX_ASSERT(g_preparedDrawBuf == NULL,
	           "nested direct array-draw reservation");
	g_preparedRingSlot = -1;
	g_preparedOversizeRingSlot = -1;
	if (count <= PS3_VTX_SLOT_MAX) {
		g_preparedRingSlot = g_vtxRingHead;
		VtxSlot &slot = g_vtxRing[g_preparedRingSlot];
		waitVtxLabel(slot.labelVal);
		g_preparedDrawBuf = slot.buf;
		g_preparedDrawOffset = slot.offset;
		g_vtxRingHead = (g_vtxRingHead + 1) % PS3_VTX_RING;
	} else {
		g_preparedOversizeRingSlot = g_vtxOversizeRingHead;
		VtxSlot &slot = g_vtxOversizeRing[g_preparedOversizeRingSlot];
		waitVtxLabel(slot.labelVal);
		g_preparedDrawBuf = slot.buf;
		g_preparedDrawOffset = slot.offset;
		g_vtxOversizeRingHead =
		    (g_vtxOversizeRingHead + 1) % PS3_OVERSIZE_RING;
	}
	return g_preparedDrawBuf;
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
	flushPendingImmediate();
	if (count <= 0) return;
	/* Snow flakes and curtains provide separate packed float position and
	 * texture-coordinate arrays, with the current normal and colour applying
	 * to every vertex. This is also a common legacy-GL layout elsewhere in
	 * the game. Select the layout once per draw instead of sending every
	 * component through readArrayComp()'s pointer/type/stride decoder. */
	const bool fastPosTexFloat =
	    g_vertexArray.enabled && g_vertexArray.pointer &&
	    g_vertexArray.type == GL_FLOAT && g_vertexArray.size == 3 &&
	    g_texCoordArray.enabled && g_texCoordArray.pointer &&
	    g_texCoordArray.type == GL_FLOAT && g_texCoordArray.size == 2 &&
	    !g_normalArray.enabled && !g_colorArray.enabled;
	const GLsizei fastPosStride =
	    g_vertexArray.stride ? g_vertexArray.stride : 3 * (GLsizei)sizeof(float);
	const GLsizei fastTexStride =
	    g_texCoordArray.stride ? g_texCoordArray.stride : 2 * (GLsizei)sizeof(float);

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
		if (fastPosTexFloat) {
			const GLint sourceFirst = first + issued;
			const u8 *pos = (const u8 *)g_vertexArray.pointer +
			                (u32)sourceFirst * (u32)fastPosStride;
			const u8 *tex = (const u8 *)g_texCoordArray.pointer +
			                (u32)sourceFirst * (u32)fastTexStride;
			for (GLint i = 0; i < batch; ++i) {
				const float *p = (const float *)pos;
				const float *t = (const float *)tex;
				ImmVtx &v = g_immVtx[g_immCount++];
				v.x = p[0]; v.y = p[1]; v.z = p[2];
				v.nx = g_curNx; v.ny = g_curNy; v.nz = g_curNz;
				v.u = t[0]; v.v = t[1];
				v.r = g_color[0]; v.g = g_color[1];
				v.b = g_color[2]; v.a = g_color[3];
				pos += fastPosStride;
				tex += fastTexStride;
			}
		} else {
			for (GLint i = 0; i < batch; i++)
				fetchVertex(first + issued + i, g_immVtx[g_immCount++]);
		}
		g_primMode = mode;
		ps3_gl_flush();
		issued += batch;
		if (!canSplit) break; /* fan/strip: one shot */
	}
	g_immCount = 0;
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
	flushPendingImmediate();
	if (count <= 0 || !indices) return;
	/* expand fully-indexed draw into de-indexed ImmVtx and reuse DrawArrays
	 * batching path. 32-bit indices are the course's preferred form. */

	/* Decode client arrays once per draw and pick a specialized gather
	 * loop. The variants below cover the float3-position + float2-texcoord
	 * combinations ETR actually uses; anything else falls back to the
	 * per-component fetchVertex() decoder. Index-type dispatch happens
	 * once per batch — the inner gather loop is branchless on type. */
	const ArrayView pos = decodeArray(g_vertexArray);
	const ArrayView nrm = decodeArray(g_normalArray);
	const ArrayView tex = decodeArray(g_texCoordArray);
	const ArrayView col = decodeArray(g_colorArray);
	bool hasNrm = false, hasColUbyte = false;
	const bool fastGather = pickGatherVariant(pos, nrm, tex, col,
	                                           hasNrm, hasColUbyte);

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
		ImmVtx *drawVertices = prepareDirectArrayDraw(batch);
		if (!drawVertices) return;
		if (fastGather) {
			/* `issued` advances the index base each batch so the inner
			 * gather can read indices[0..batch-1] directly. */
			if (hasNrm && hasColUbyte) {
				if (type == GL_UNSIGNED_INT)
					gatherIndexed<true, true, GLuint>(pos, nrm, tex, col, batch, (const GLuint *)indices + issued, drawVertices);
				else if (type == GL_UNSIGNED_SHORT)
					gatherIndexed<true, true, GLushort>(pos, nrm, tex, col, batch, (const GLushort *)indices + issued, drawVertices);
				else
					gatherIndexed<true, true, GLubyte>(pos, nrm, tex, col, batch, (const GLubyte *)indices + issued, drawVertices);
			} else if (hasNrm) {
				if (type == GL_UNSIGNED_INT)
					gatherIndexed<true, false, GLuint>(pos, nrm, tex, col, batch, (const GLuint *)indices + issued, drawVertices);
				else if (type == GL_UNSIGNED_SHORT)
					gatherIndexed<true, false, GLushort>(pos, nrm, tex, col, batch, (const GLushort *)indices + issued, drawVertices);
				else
					gatherIndexed<true, false, GLubyte>(pos, nrm, tex, col, batch, (const GLubyte *)indices + issued, drawVertices);
			} else {
				if (type == GL_UNSIGNED_INT)
					gatherIndexed<false, false, GLuint>(pos, nrm, tex, col, batch, (const GLuint *)indices + issued, drawVertices);
				else if (type == GL_UNSIGNED_SHORT)
					gatherIndexed<false, false, GLushort>(pos, nrm, tex, col, batch, (const GLushort *)indices + issued, drawVertices);
				else
					gatherIndexed<false, false, GLubyte>(pos, nrm, tex, col, batch, (const GLubyte *)indices + issued, drawVertices);
			}
			g_immCount = batch;
		} else {
			for (GLsizei i = 0; i < batch; i++) {
				GLuint idx;
				if (type == GL_UNSIGNED_INT)
					idx = ((const GLuint *)indices)[issued + i];
				else if (type == GL_UNSIGNED_SHORT)
					idx = ((const GLushort *)indices)[issued + i];
				else
					idx = ((const GLubyte *)indices)[issued + i];
				fetchVertex((GLint)idx, drawVertices[g_immCount++]);
			}
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
	flushPendingImmediate();
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
	g_dirtyBits |= DIRTY_TEXTURE;
}

void glBindTexture(GLenum, GLuint t) {
	GLuint next = g_currentTex;
	if (t > 0 && t < PS3_MAX_TEXTURES) next = t;
	else if (t == 0) next = 0;
	if (next == g_currentTex) return;
	flushPendingImmediate();
	g_currentTex = next;
	g_dirtyBits |= DIRTY_TEXTURE;
}

void glTexImage2D(GLenum, GLint, GLint, GLsizei w, GLsizei h, GLint,
                  GLenum, GLenum, const GLvoid *data) {
	flushPendingImmediate();
	if (g_currentTex == 0 || g_currentTex >= PS3_MAX_TEXTURES) return;
	TIMER_START("TEXIMAGE2D");
	/* RSX caps A8R8G8B8 dimensions at 4096 and needs w,h > 0. A 0 dim
	 * would pitch-align to 0 and let rsxMemalign hand back NULL — the
	 * assert fires earlier so the call site shows in the TTY capture. */
	GFX_ASSERT(w > 0 && h > 0, "glTexImage2D called with zero dim");
	GFX_ASSERT(w <= 4096 && h <= 4096, "glTexImage2D dim exceeds RSX 4096 limit");

	GlTex &T = g_tex[g_currentTex];
	T.used = GL_TRUE;
	T.width = w; T.height = h;

	const u32 srcRowBytes = (u32)w * 4u;
	const u32 pitch = (srcRowBytes + 63u) & ~63u;
	T.pitch = pitch;
	GFX_ASSERT(GFX_IS_MULT(pitch, 64), "texture pitch not 64-aligned");

	if (T.buffer) rsxFree(T.buffer);
	T.buffer = (u8 *)rsxMemalign(128, (u32)pitch * (u32)h);
	if (!T.buffer) { sysTtyTrace("[etr] glTexImage2D: rsxMemalign FAILED\n"); TIMER_END("TEXIMAGE2D"); return; }
	GFX_ASSERT_ALIGNED(T.buffer, 128);

	const u8 *src = (const u8 *)data;
	if (src) {
		/* RGBA→A8R8G8B8 byte permutation: dst[0..3] = src[3,0,1,2].
		 * vec_perm(out[i]) = in[perm[i]], so the pattern picks bytes
		 * {A,R,G,B} from each 4-byte pixel. One constant covers four
		 * pixels per vector. */
		static const vector unsigned char kArgbSwizzle =
		    (vector unsigned char){3, 0, 1, 2,
		                           7, 4, 5, 6,
		                           11, 8, 9, 10,
		                           15, 12, 13, 14};
		/* The unaligned-load idiom (vec_lvsl + two vec_ld + vec_perm)
		 * reads 32 bytes from the aligned base containing each chunk's
		 * start. To guarantee that span stays within the source on the
		 * last row, SIMD processes at most (w-4)/4 four-pixel chunks;
		 * the remaining pixels fall through to the scalar tail. For
		 * w <= 4 the SIMD loop is skipped entirely. */
		const GLsizei simdChunks = (w >= 4) ? (w - 4) / 4 : 0;
		const GLsizei simdPixels = simdChunks * 4;

		for (GLsizei y = 0; y < h; y++) {
			const u8 *srow = src + y * srcRowBytes;
			u8 *drow = T.buffer + (u32)y * pitch;
			for (GLsizei c = 0; c < simdChunks; c++) {
				const u8 *sp = srow + c * 16;
				const vector unsigned char perm = vec_lvsl(0, sp);
				const vector unsigned char hi  = vec_ld(0,  sp);
				const vector unsigned char lo  = vec_ld(16, sp);
				const vector unsigned char v   = vec_perm(hi, lo, perm);
				const vector unsigned char out = vec_perm(v, v, kArgbSwizzle);
				/* drow is 64-aligned (T.buffer 128-aligned, pitch a
				 * multiple of 64); c*16 is 16-aligned. */
				vec_st(out, c * 16, drow);
			}
			for (GLsizei x = simdPixels; x < w; x++) {
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
	GFX_ASSERT(rsxAddressToOffset(T.buffer, &T.offset) == 0,
	           "rsxAddressToOffset(texture) failed");
	/* GCM_TEXTURE_FORMAT_A8R8G8B8 | LIN needs a 128-aligned base offset
	 * on real RSX; the rsxMemalign(128, ...) above guarantees it but
	 * catch any future regression in allocation alignment. */
	GFX_ASSERT_ALIGNED(T.offset, 128);
	g_dirtyBits |= DIRTY_TEXTURE;
	TIMER_END("TEXIMAGE2D");
}

void glTexParameteri(GLenum, GLenum pname, GLint param) {
	if (g_currentTex == 0 || g_currentTex >= PS3_MAX_TEXTURES) return;
	GlTex &T = g_tex[g_currentTex];
	switch (pname) {
		case GL_TEXTURE_MIN_FILTER:
		case GL_TEXTURE_MAG_FILTER: {
			GLboolean smooth =
			    (param == GL_LINEAR || param == GL_LINEAR_MIPMAP_LINEAR ||
			     param == GL_NEAREST_MIPMAP_LINEAR) ? GL_TRUE : GL_FALSE;
			if (T.smooth == smooth) return;
			flushPendingImmediate();
			T.smooth = smooth;
			break;
		}
		case GL_TEXTURE_WRAP_S:
		case GL_TEXTURE_WRAP_T: {
			GLboolean repeated = (param == GL_REPEAT) ? GL_TRUE : GL_FALSE;
			if (T.repeated == repeated) return;
			flushPendingImmediate();
			T.repeated = repeated;
			break;
		}
		default: return;
	}
	g_dirtyBits |= DIRTY_TEXTURE;
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
	flushPendingImmediate();
	if (!g_rsxReady) return;
	TIMER_START("CLEAR");
	u32 col = ((u32)(g_clearA * 255.f) & 0xFF) << 24 |
	          ((u32)(g_clearR * 255.f) & 0xFF) << 16 |
	          ((u32)(g_clearG * 255.f) & 0xFF) << 8  |
	          ((u32)(g_clearB * 255.f) & 0xFF);
	rsxSetClearColor(context, col);
	rsxSetClearDepthStencil(context, 0xffffff00u | (g_clearStencil & 0xFF));
	rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
	                        GCM_CLEAR_S | GCM_CLEAR_Z);
	TIMER_END("CLEAR");
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
void glGetFloatv(GLenum pname, GLfloat *p) {
	switch (pname) {
	case GL_MODELVIEW_MATRIX:
		memcpy(p, g_mv.m, sizeof(float) * 16);
		break;
	case GL_PROJECTION_MATRIX:
		memcpy(p, g_proj.m, sizeof(float) * 16);
		break;
	default:
		*p = 0.f;
		break;
	}
}
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
extern "C" void  glXSwapBuffers(void *, unsigned long) { ps3_gl_flush_pending(); }

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

	g_terrainVpo = (rsxVertexProgram *)etr_terrain_vpo;
	u32 terrainVpSize = 0;
	rsxVertexProgramGetUCode(g_terrainVpo, &g_terrainVpUcode,
	                        &terrainVpSize);
	GFX_ASSERT(terrainVpSize > 0, "terrain vertex program has zero size");
	g_tvProj          = rsxVertexProgramGetConst(g_terrainVpo, "projMatrix");
	g_tvMV            = rsxVertexProgramGetConst(g_terrainVpo, "modelViewMatrix");
	g_tvTexPlaneS     = rsxVertexProgramGetConst(g_terrainVpo, "texPlaneS");
	g_tvTexPlaneT     = rsxVertexProgramGetConst(g_terrainVpo, "texPlaneT");
	g_tvDoTexGen      = rsxVertexProgramGetConst(g_terrainVpo, "doTexGen");
	g_tvGlobalAmbient = rsxVertexProgramGetConst(g_terrainVpo, "globalAmbient");
	g_tvLightPos      = rsxVertexProgramGetConst(g_terrainVpo, "lightPosition");
	g_tvLightColor    = rsxVertexProgramGetConst(g_terrainVpo, "lightColor");
	g_tvLightIsDir    = rsxVertexProgramGetConst(g_terrainVpo, "lightIsDir");
	g_tvMatDiffuse    = rsxVertexProgramGetConst(g_terrainVpo, "matDiffuse");

	g_fpo = (rsxFragmentProgram *)etr3d_fpo;
	rsxFragmentProgramGetUCode(g_fpo, &g_fpUcode, &g_fpUcodeSize);

	g_terrainFpo = (rsxFragmentProgram *)etr_terrain_fpo;
	rsxFragmentProgramGetUCode(g_terrainFpo, &g_terrainFpUcode,
	                           &g_terrainFpUcodeSize);
	GFX_ASSERT(g_terrainFpUcodeSize > 0,
	           "terrain fragment program has zero size");

	g_uiFpo = (rsxFragmentProgram *)etr_ui_fpo;
	u32 uiFpSize = 0;
	rsxFragmentProgramGetUCode(g_uiFpo, &g_uiFpUcode, &uiFpSize);
	GFX_ASSERT(uiFpSize > 0, "UI fragment program has zero size");
	g_uiFpBuf = (u32 *)rsxMemalign(64, uiFpSize);
	if (!g_uiFpBuf) {
		sysTtyTrace("[etr] ps3_gl_init: UI fragment-program alloc FAILED\n");
		return;
	}
	GFX_ASSERT_ALIGNED(g_uiFpBuf, 64);
	memcpy(g_uiFpBuf, g_uiFpUcode, uiFpSize);
	GFX_ASSERT(rsxAddressToOffset(g_uiFpBuf, &g_uiFpOffset) == 0,
	           "rsxAddressToOffset(g_uiFpBuf) failed");
	GFX_ASSERT_ALIGNED(g_uiFpOffset, 64);
	g_uiTexSampler = rsxFragmentProgramGetAttrib(g_uiFpo, "texture");

	/* Ring of fragment-program buffers (same count as the vertex ring so they
	 * share the same fence labels). Each slot starts as a copy of the clean
	 * ucode; constants are patched into the acquired slot at flush time. */
	for (int i = 0; i < PS3_VTX_RING; i++) {
		g_fpRing[i].buf = (u32 *)rsxMemalign(64, g_fpUcodeSize);
		if (!g_fpRing[i].buf) {
			sysTtyTrace("[etr] ps3_gl_init: fp ring alloc FAILED\n");
			return;
		}
		GFX_ASSERT_ALIGNED(g_fpRing[i].buf, 64);
		memcpy(g_fpRing[i].buf, g_fpUcode, g_fpUcodeSize);
		GFX_ASSERT(rsxAddressToOffset(g_fpRing[i].buf, &g_fpRing[i].offset) == 0,
		           "rsxAddressToOffset(fp ring) failed");
		GFX_ASSERT_ALIGNED(g_fpRing[i].offset, 64);
		g_fpRing[i].labelVal = 0;
		g_fpRing[i].generation = 0;

		g_terrainFpRing[i].buf =
		    (u32 *)rsxMemalign(64, g_terrainFpUcodeSize);
		if (!g_terrainFpRing[i].buf) {
			sysTtyTrace("[etr] ps3_gl_init: terrain fp ring alloc FAILED\n");
			return;
		}
		GFX_ASSERT_ALIGNED(g_terrainFpRing[i].buf, 64);
		memcpy(g_terrainFpRing[i].buf, g_terrainFpUcode,
		       g_terrainFpUcodeSize);
		GFX_ASSERT(rsxAddressToOffset(g_terrainFpRing[i].buf,
		                             &g_terrainFpRing[i].offset) == 0,
		           "rsxAddressToOffset(terrain fp ring) failed");
		GFX_ASSERT_ALIGNED(g_terrainFpRing[i].offset, 64);
		g_terrainFpRing[i].labelVal = 0;
		g_terrainFpRing[i].generation = 0;
	}
	g_fpRingHead = g_terrainFpRingHead = 0;
	g_currentFpSlot = g_currentTerrainFpSlot = NULL;

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
	g_uOutputScale   = rsxFragmentProgramGetConst(g_fpo, "outputScale");
	g_texSampler     = rsxFragmentProgramGetAttrib(g_fpo, "texture");

	g_tFogColor      = rsxFragmentProgramGetConst(g_terrainFpo, "fogColor");
	g_tFogSE         = rsxFragmentProgramGetConst(g_terrainFpo, "fogSE");
	g_tDoFog         = rsxFragmentProgramGetConst(g_terrainFpo, "doFog");
	g_tOutputScale   = rsxFragmentProgramGetConst(g_terrainFpo, "outputScale");
	g_tTexSampler    = rsxFragmentProgramGetAttrib(g_terrainFpo, "texture");

	if (!g_uProj) sysTtyTrace("[etr] ps3_gl_init: WARN projMatrix missing\n");
	if (!g_uMV)   sysTtyTrace("[etr] ps3_gl_init: WARN modelViewMatrix missing\n");
	/* The shader is built with these uniforms always present; a NULL lookup
	 * means the etr3d.vpo embedded in the build is out of sync with the
	 * Cg source — every draw would upload junk transforms. */
	GFX_ASSERT(g_uProj != NULL, "etr3d vertex shader missing projMatrix");
	GFX_ASSERT(g_uMV   != NULL, "etr3d vertex shader missing modelViewMatrix");

	g_whiteBuf = (u32 *)rsxMemalign(128, 4);
	GFX_ASSERT(g_whiteBuf != NULL, "whiteBuf alloc failed");
	GFX_ASSERT_ALIGNED(g_whiteBuf, 128);
	g_whiteBuf[0] = 0xFFFFFFFFu;
	GFX_ASSERT(rsxAddressToOffset(g_whiteBuf, &g_whiteOffset) == 0,
	           "rsxAddressToOffset(whiteBuf) failed");
	/* 1x1 A8R8G8B8 texture base offset must be 128-aligned on real RSX. */
	GFX_ASSERT_ALIGNED(g_whiteOffset, 128);

	for (int i = 0; i < PS3_VTX_RING; i++) {
		g_vtxRing[i].buf = (ImmVtx *)rsxMemalign(64, sizeof(ImmVtx) * PS3_VTX_SLOT_MAX);
		if (!g_vtxRing[i].buf) {
			sysTtyTrace("[etr] ps3_gl_init: vtx ring alloc FAILED\n");
			return;
		}
		GFX_ASSERT_ALIGNED(g_vtxRing[i].buf, 64);
		/* Stride must be a multiple of 4 for F32 attribs — true today
		 * (48 bytes) but cheap to catch if the ImmVtx layout changes. */
		GFX_ASSERT(GFX_IS_MULT(sizeof(ImmVtx), 4), "ImmVtx stride must be 4-aligned");
		GFX_ASSERT(rsxAddressToOffset(g_vtxRing[i].buf, &g_vtxRing[i].offset) == 0,
		           "rsxAddressToOffset(vtx ring) failed");
		GFX_ASSERT_ALIGNED(g_vtxRing[i].offset, 64);
		g_vtxRing[i].labelVal = 0;
	}
	g_vtxRingHead = 0;
	for (int i = 0; i < PS3_OVERSIZE_RING; ++i) {
		VtxSlot &slot = g_vtxOversizeRing[i];
		slot.buf = (ImmVtx *)rsxMemalign(64,
		                                  sizeof(ImmVtx) * PS3_IMM_MAX);
		if (!slot.buf) {
			sysTtyTrace("[etr] ps3_gl_init: vtx oversize ring alloc FAILED\n");
			return;
		}
		GFX_ASSERT_ALIGNED(slot.buf, 64);
		GFX_ASSERT(rsxAddressToOffset(slot.buf, &slot.offset) == 0,
		           "rsxAddressToOffset(vtx oversize ring) failed");
		GFX_ASSERT_ALIGNED(slot.offset, 64);
		slot.labelVal = 0;
	}
	g_vtxOversizeRingHead = 0;

	g_vtxLabel = (vu32 *)gcmGetLabelAddress(PS3_VTX_LABEL_IDX);
	*g_vtxLabel = 0;
	g_vtxLabelNext = 1;
	g_unflushedDraws = 0;

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
	if (g_dirtyBits & DIRTY_FIXED_ENV) {
		rsxSetColorMask(context, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G |
		                         GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
		rsxSetColorMaskMrt(context, 0);
		rsxSetFrontFace(context, GCM_FRONTFACE_CCW);
		rsxSetShadeModel(context, GCM_SHADE_MODEL_SMOOTH);
		rsxSetLogicOpEnable(context, GCM_FALSE);
		rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
		/* Match OpenGL's clip-volume behavior. Ignoring W while clamping Z
		 * lets behind-camera vertices survive the perspective divide. */
		rsxSetZMinMaxControl(context, GCM_TRUE, GCM_FALSE, GCM_FALSE);
	}

	if (g_dirtyBits & DIRTY_VIEWPORT) {
		const u16 x = 0, y = 0;
		const u16 w = display_width, h = display_height;
		const float min = 0.f, max = 1.f;
		float scale[4], offset[4];
		scale[0] = w * 0.5f; scale[1] = h * -0.5f;
		scale[2] = (max - min) * 0.5f; scale[3] = 0.f;
		offset[0] = x + w * 0.5f; offset[1] = y + h * 0.5f;
		offset[2] = (max + min) * 0.5f; offset[3] = 0.f;
		rsxSetViewport(context, x, y, w, h, min, max, scale, offset);
		rsxSetScissor(context, x, y, w, h);
		for (u8 i = 0; i < 8; i++)
			rsxSetViewportClip(context, i, display_width, display_height);
	}

	if (g_dirtyBits & DIRTY_DEPTH) {
		/* GCM depth-func enums match the GL values we store. */
		rsxSetDepthTestEnable(context, g_depthTest ? GCM_TRUE : GCM_FALSE);
		rsxSetDepthFunc(context, g_depthFunc);
		rsxSetDepthWriteEnable(context, g_depthMask ? 1 : 0);
	}

	if (g_dirtyBits & DIRTY_CULL) {
		rsxSetCullFaceEnable(context, g_cullFace ? GCM_TRUE : GCM_FALSE);
		if (g_cullFace) rsxSetCullFace(context, GCM_CULL_BACK);
	}

	if (g_dirtyBits & DIRTY_BLEND) {
		/* Avoid destination reads for opaque course, sky, and cutout passes. */
		const GLboolean opaqueCourse =
		    g_lighting && g_depthTest && g_depthMask && g_cullFace &&
		    !g_alphaTest && g_texGenS && g_texGenT &&
		    g_blendSrc == GL_SRC_ALPHA &&
		    g_blendDst == GL_ONE_MINUS_SRC_ALPHA;
		const GLboolean opaqueSky =
		    !g_lighting && !g_depthTest && !g_depthMask && !g_alphaTest &&
		    !g_texGenS && !g_texGenT &&
		    g_blendSrc == GL_SRC_ALPHA &&
		    g_blendDst == GL_ONE_MINUS_SRC_ALPHA;
		const GLboolean opaqueCutout =
		    g_alphaTest && g_alphaFunc == GL_GEQUAL &&
		    g_depthTest && g_depthMask &&
		    g_blendSrc == GL_SRC_ALPHA &&
		    g_blendDst == GL_ONE_MINUS_SRC_ALPHA;
		const GLboolean effectiveBlend =
		    g_blend && !opaqueCourse && !opaqueSky && !opaqueCutout;
		rsxSetBlendFunc(context,
		                effectiveBlend ? g_blendSrc : GCM_ONE,
		                effectiveBlend ? g_blendDst : GCM_ZERO,
		                effectiveBlend ? g_blendSrc : GCM_ONE,
		                effectiveBlend ? g_blendDst : GCM_ZERO);
		rsxSetBlendEnable(context, effectiveBlend ? GCM_TRUE : GCM_FALSE);
	}

	if (g_dirtyBits & DIRTY_ALPHA) {
		if (g_alphaTest && g_alphaFunc == GL_GEQUAL) {
			rsxSetAlphaFunc(context, GCM_GEQUAL,
			                (u32)(g_alphaRef * 255.f));
			rsxSetAlphaTestEnable(context, GCM_TRUE);
		} else {
			rsxSetAlphaTestEnable(context, GCM_FALSE);
		}
	}
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

	/* rsxLoadTexture with LIN A8R8G8B8 requires: base offset 128-aligned,
	 * pitch a multiple of 64, tw/th > 0 and <= 4096. Real RSX faults with
	 * garbage sampling (the classic source of "blown up" textured polys)
	 * where RPCS3 quietly rounds. */
	GFX_ASSERT_ALIGNED(offset, 128);
	GFX_ASSERT(GFX_IS_MULT(pitch, 64) || pitch == 4,
	           "texture pitch not 64-aligned (except 1x1 white=4)");
	GFX_ASSERT(tw > 0 && th > 0, "texture dims zero at draw time");
	GFX_ASSERT(tw <= 4096 && th <= 4096, "texture dims exceed 4096");

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

	GLboolean uiDraw = useUiFragmentProgram();
	GLboolean liteDraw = useLiteFragmentProgram();
	u8 unit = uiDraw && g_uiTexSampler ? g_uiTexSampler->index :
	          liteDraw && g_tTexSampler ? g_tTexSampler->index :
	          g_texSampler ? g_texSampler->index : 0;
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

static void submitPendingRsxCommands(void) {
	if (g_unflushedDraws == 0) return;
	rsxFlushBuffer(context);
	g_unflushedDraws = 0;
}

static void waitVtxLabel(u32 val) {
	if (val == 0 || !g_vtxLabel) return;
	/* A label cannot advance until its command buffer has been published.
	 * Flush here before a ring reuse can block; ordinary draws publish in
	 * larger groups below. */
	submitPendingRsxCommands();
	TIMER_START("GL_FLUSH_WAIT");
	while ((s32)(*g_vtxLabel - val) < 0)
		usleep(10);
	TIMER_END("GL_FLUSH_WAIT");
}

extern "C" void ps3_gl_flush(void) {
	if (g_immCount == 0 || !g_rsxReady) return;

	int n = g_immCount;
	if (n > PS3_IMM_MAX) n = PS3_IMM_MAX;

	u32 drawOffset;
	ImmVtx *drawBuf;
	int ringSlot = -1;
	int oversizeRingSlot = -1;
	FpSlot *activeFpSlot = NULL;
	u32  fpOffset = 0;
	GLboolean patchFragmentProgram = GL_FALSE;
	const GLboolean terrainDraw = useTerrainFragmentProgram();
	const GLboolean uiDraw = useUiFragmentProgram();
	const GLboolean liteDraw = useLiteFragmentProgram();
	const GLboolean drawAlreadyPrepared =
	    g_preparedDrawBuf ? GL_TRUE : GL_FALSE;

	if (drawAlreadyPrepared) {
		drawBuf = g_preparedDrawBuf;
		drawOffset = g_preparedDrawOffset;
		ringSlot = g_preparedRingSlot;
		oversizeRingSlot = g_preparedOversizeRingSlot;
		g_preparedDrawBuf = NULL;
		g_preparedDrawOffset = 0;
		g_preparedRingSlot = -1;
		g_preparedOversizeRingSlot = -1;
	} else if (n <= PS3_VTX_SLOT_MAX) {
		/* Acquire next ring slot for BOTH vertex data and the fragment-program
		 * ucode copy. Waiting on the same label keeps the two in lockstep. */
		ringSlot = g_vtxRingHead;
		VtxSlot &slot = g_vtxRing[ringSlot];
		waitVtxLabel(slot.labelVal);
		drawBuf    = slot.buf;
		drawOffset = slot.offset;
		g_vtxRingHead = (g_vtxRingHead + 1) % PS3_VTX_RING;
	} else {
		/* Large strips/fans cannot be split without preserving topology. Keep
		 * them asynchronous in a smaller ring instead of draining the whole
		 * RSX before every large Tux sphere. */
		oversizeRingSlot = g_vtxOversizeRingHead;
		VtxSlot &slot = g_vtxOversizeRing[oversizeRingSlot];
		waitVtxLabel(slot.labelVal);
		drawBuf    = slot.buf;
		drawOffset = slot.offset;
		g_vtxOversizeRingHead =
		    (g_vtxOversizeRingHead + 1) % PS3_OVERSIZE_RING;
	}

	/* Fragment-program storage rotates only when its constants change. The
	 * selected slot remains immutable and can be shared by any number of
	 * consecutive draws without being tied to the vertex-ring cadence. */
	if (liteDraw) {
		if (!g_currentTerrainFpSlot ||
		    g_currentTerrainFpSlot->generation != g_liteFpGeneration) {
			FpSlot &slot = g_terrainFpRing[g_terrainFpRingHead];
			waitVtxLabel(slot.labelVal);
			g_currentTerrainFpSlot = &slot;
			g_terrainFpRingHead = (g_terrainFpRingHead + 1) % PS3_VTX_RING;
			patchFragmentProgram = GL_TRUE;
		}
		activeFpSlot = g_currentTerrainFpSlot;
	} else if (!uiDraw) {
		if (!g_currentFpSlot ||
		    g_currentFpSlot->generation != g_fullFpGeneration) {
			FpSlot &slot = g_fpRing[g_fpRingHead];
			waitVtxLabel(slot.labelVal);
			g_currentFpSlot = &slot;
			g_fpRingHead = (g_fpRingHead + 1) % PS3_VTX_RING;
			patchFragmentProgram = GL_TRUE;
		}
		activeFpSlot = g_currentFpSlot;
	}
	if (!uiDraw) {
		if (!activeFpSlot || !activeFpSlot->buf || !activeFpSlot->offset)
			return;
		fpOffset = activeFpSlot->offset;
	}

	TIMER_START("GL_FLUSH");

	/* Pre-draw invariants. These are the conditions real RSX enforces
	 * but RPCS3 papers over — catching them here freezes the frame on
	 * the actual offending draw instead of letting corruption propagate
	 * through the rest of the scene. */
	GFX_ASSERT(n > 0, "ps3_gl_flush with no vertices");
	GFX_ASSERT(drawBuf != NULL, "drawBuf NULL at flush");
	GFX_ASSERT_ALIGNED(drawBuf, 64);
	GFX_ASSERT_ALIGNED(drawOffset, 64);
	if (!uiDraw) GFX_ASSERT_ALIGNED(fpOffset, 64);
	/* Stride must be a multiple of 4 for F32 attribs (true at 48). */
	GFX_ASSERT(GFX_IS_MULT(sizeof(ImmVtx), 4), "stride not 4-aligned");
	/* Note: we deliberately do NOT assert on primitive-count-vs-mode here.
	 * GL defines incomplete primitives (e.g. GL_TRIANGLE_FAN with < 3
	 * verts, GL_QUADS with non-multiple-of-4) as rendering nothing, and
	 * the game relies on this — draw_partial_tri_fan emits a center +
	 * variable loop count, so a near-empty gauge flushes a 1–2 vert
	 * fan. RSX also produces nothing for those cases. The strict checks
	 * here caused false halts during gameplay. */

	if (!drawAlreadyPrepared)
		memcpy(drawBuf, g_immVtx, sizeof(ImmVtx) * n);
	/* Publish PPU stores to RSX-visible local memory before the invalidate
	 * and draw commands can reach the GPU.  RPCS3 presents coherent memory,
	 * but real hardware can otherwise fetch a mixture of old/new cache lines
	 * from a freshly refilled ring slot, producing stretched triangles. */
	asm volatile("sync");

	if (g_dirtyBits & DIRTY_DRAW_ENV) setDrawEnv();
	if (g_dirtyBits & DIRTY_TEXTURE)  bindTextureForDraw();

	/* POS / NRM / TEX0 / COL attribs.
	 * F32 attribs require each per-vertex offset to be 4-byte aligned;
	 * drawOffset is 64-aligned so the struct field offsets (0/12/24/32)
	 * keep every element aligned. Checked explicitly because a future
	 * ImmVtx field reorder would silently misbind on real hardware. */
	const u32 stride = sizeof(ImmVtx);
	GFX_ASSERT_ALIGNED(drawOffset + 0,                  4);
	GFX_ASSERT_ALIGNED(drawOffset + sizeof(float) * 3,  4);
	GFX_ASSERT_ALIGNED(drawOffset + sizeof(float) * 6,  4);
	GFX_ASSERT_ALIGNED(drawOffset + sizeof(float) * 8,  4);
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

	/* Vertex program + uniforms (transposed matrices). A program switch forces
	 * every constant into the new program; otherwise only state changed since
	 * the preceding draw is emitted. */
	alignas(16) float tmp[16];
	const VertexProgramKind wantedVertexProgram =
	    terrainDraw ? VP_TERRAIN : VP_GENERAL;
	const GLboolean vertexProgramChanged =
	    g_loadedVertexProgram != wantedVertexProgram;
	if (terrainDraw) {
		if (vertexProgramChanged)
			rsxLoadVertexProgram(context, g_terrainVpo, g_terrainVpUcode);
		if (g_tvProj &&
		    (vertexProgramChanged || (g_dirtyBits & DIRTY_PROJ_MATRIX))) {
			matTranspose(tmp, g_proj.m);
			rsxSetVertexProgramParameter(context, g_terrainVpo, g_tvProj, tmp);
		}
		if (g_tvMV &&
		    (vertexProgramChanged || (g_dirtyBits & DIRTY_MV_MATRIX))) {
			matTranspose(tmp, g_mv.m);
			rsxSetVertexProgramParameter(context, g_terrainVpo, g_tvMV, tmp);
		}
		if (vertexProgramChanged || (g_dirtyBits & DIRTY_TEXGEN)) {
			float doTexGen = (g_texGenS || g_texGenT) ? 1.f : 0.f;
			if (g_tvDoTexGen)  rsxSetVertexProgramParameter(context, g_terrainVpo, g_tvDoTexGen, &doTexGen);
			if (g_tvTexPlaneS) rsxSetVertexProgramParameter(context, g_terrainVpo, g_tvTexPlaneS, g_texPlaneS);
			if (g_tvTexPlaneT) rsxSetVertexProgramParameter(context, g_terrainVpo, g_tvTexPlaneT, g_texPlaneT);
		}

		if (vertexProgramChanged || (g_dirtyBits & DIRTY_VP_LIGHTING)) {
			float ambient[3], lightPos[3], lightDiff[3], lightSpec[3], isDir;
			composeLighting(ambient, lightPos, lightDiff, lightSpec, &isDir);
			float matDiff[3] = {
				g_matDiffuse[0], g_matDiffuse[1], g_matDiffuse[2]
			};
			if (g_tvGlobalAmbient) rsxSetVertexProgramParameter(context, g_terrainVpo, g_tvGlobalAmbient, ambient);
			if (g_tvLightPos)      rsxSetVertexProgramParameter(context, g_terrainVpo, g_tvLightPos, lightPos);
			if (g_tvLightColor)    rsxSetVertexProgramParameter(context, g_terrainVpo, g_tvLightColor, lightDiff);
			if (g_tvLightIsDir)    rsxSetVertexProgramParameter(context, g_terrainVpo, g_tvLightIsDir, &isDir);
			if (g_tvMatDiffuse)    rsxSetVertexProgramParameter(context, g_terrainVpo, g_tvMatDiffuse, matDiff);
		}
	} else {
		if (vertexProgramChanged)
			rsxLoadVertexProgram(context, g_vpo, g_vpUcode);
		if (g_uProj &&
		    (vertexProgramChanged || (g_dirtyBits & DIRTY_PROJ_MATRIX))) {
			matTranspose(tmp, g_proj.m);
			rsxSetVertexProgramParameter(context, g_vpo, g_uProj, tmp);
		}
		if (g_uMV &&
		    (vertexProgramChanged || (g_dirtyBits & DIRTY_MV_MATRIX))) {
			matTranspose(tmp, g_mv.m);
			rsxSetVertexProgramParameter(context, g_vpo, g_uMV, tmp);
		}
		if (vertexProgramChanged || (g_dirtyBits & DIRTY_TEXGEN)) {
			float doTexGen = (g_texGenS || g_texGenT) ? 1.f : 0.f;
			if (g_uDoTexGen)  rsxSetVertexProgramParameter(context, g_vpo, g_uDoTexGen, &doTexGen);
			if (g_uTexPlaneS) rsxSetVertexProgramParameter(context, g_vpo, g_uTexPlaneS, g_texPlaneS);
			if (g_uTexPlaneT) rsxSetVertexProgramParameter(context, g_vpo, g_uTexPlaneT, g_texPlaneT);
		}
	}
	g_loadedVertexProgram = wantedVertexProgram;
	if (uiDraw) {
		/* The immutable UI shader has no per-draw constants.  Patching the
		 * unused 3D lighting/fog program here used to emit fifteen inline
		 * transfers for every HUD quad before immediately loading etr_ui. */
		if (g_loadedFragmentProgram != FP_UI ||
		    g_loadedFragmentOffset != g_uiFpOffset) {
			rsxLoadFragmentProgramLocation(context, g_uiFpo, g_uiFpOffset,
			                               GCM_LOCATION_RSX);
			g_loadedFragmentProgram = FP_UI;
			g_loadedFragmentOffset = g_uiFpOffset;
		}
	} else if (liteDraw) {
		if (patchFragmentProgram) {
			float fogCol[3] = { g_fogColor[0], g_fogColor[1], g_fogColor[2] };
			float fogSE[2] = { g_fogStart, g_fogEnd };
			float doFog = g_fog ? 1.f : 0.f;
			float outputScale[4] = {1.f, 1.f, 1.f, 1.f};

			TIMER_START("FLUSH_FP_PATCHES");
			if (g_tFogColor)      rsxSetFragmentProgramParameter(context, g_terrainFpo, g_tFogColor, fogCol, fpOffset, GCM_LOCATION_RSX);
			if (g_tFogSE)         rsxSetFragmentProgramParameter(context, g_terrainFpo, g_tFogSE, fogSE, fpOffset, GCM_LOCATION_RSX);
			if (g_tDoFog)         rsxSetFragmentProgramParameter(context, g_terrainFpo, g_tDoFog, &doFog, fpOffset, GCM_LOCATION_RSX);
			if (g_tOutputScale)   rsxSetFragmentProgramParameter(context, g_terrainFpo, g_tOutputScale, outputScale, fpOffset, GCM_LOCATION_RSX);
			TIMER_END("FLUSH_FP_PATCHES");
			activeFpSlot->generation = g_liteFpGeneration;
		}

		if (patchFragmentProgram ||
		    g_loadedFragmentProgram != FP_LITE ||
		    g_loadedFragmentOffset != fpOffset) {
			rsxLoadFragmentProgramLocation(context, g_terrainFpo, fpOffset,
			                               GCM_LOCATION_RSX);
			g_loadedFragmentProgram = FP_LITE;
			g_loadedFragmentOffset = fpOffset;
		}
	} else {
		if (patchFragmentProgram) {
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
			float outputScale[4] = {1.f, 1.f, 1.f, 1.f};

			TIMER_START("FLUSH_FP_PATCHES");
			if (g_uGlobalAmbient) rsxSetFragmentProgramParameter(context, g_fpo, g_uGlobalAmbient, ambient, fpOffset, GCM_LOCATION_RSX);
			if (g_uLightPos)      rsxSetFragmentProgramParameter(context, g_fpo, g_uLightPos, lightPos, fpOffset, GCM_LOCATION_RSX);
			if (g_uLightColor)    rsxSetFragmentProgramParameter(context, g_fpo, g_uLightColor, lightDiff, fpOffset, GCM_LOCATION_RSX);
			if (g_uLightSpec)     rsxSetFragmentProgramParameter(context, g_fpo, g_uLightSpec, lightSpec, fpOffset, GCM_LOCATION_RSX);
			if (g_uLightIsDir)    rsxSetFragmentProgramParameter(context, g_fpo, g_uLightIsDir, &isDir, fpOffset, GCM_LOCATION_RSX);
			if (g_uMatDiffuse)    rsxSetFragmentProgramParameter(context, g_fpo, g_uMatDiffuse, matDiff, fpOffset, GCM_LOCATION_RSX);
			if (g_uMatSpecular)   rsxSetFragmentProgramParameter(context, g_fpo, g_uMatSpecular, matSpec, fpOffset, GCM_LOCATION_RSX);
			if (g_uShininess)     rsxSetFragmentProgramParameter(context, g_fpo, g_uShininess, &shin, fpOffset, GCM_LOCATION_RSX);
			if (g_uDoLighting)    rsxSetFragmentProgramParameter(context, g_fpo, g_uDoLighting, &doLighting, fpOffset, GCM_LOCATION_RSX);
			if (g_uFogColor)      rsxSetFragmentProgramParameter(context, g_fpo, g_uFogColor, fogCol, fpOffset, GCM_LOCATION_RSX);
			if (g_uFogSE)         rsxSetFragmentProgramParameter(context, g_fpo, g_uFogSE, fogSE, fpOffset, GCM_LOCATION_RSX);
			if (g_uDoFog)         rsxSetFragmentProgramParameter(context, g_fpo, g_uDoFog, &doFog, fpOffset, GCM_LOCATION_RSX);
			if (g_uAlphaRef)      rsxSetFragmentProgramParameter(context, g_fpo, g_uAlphaRef, &aRef, fpOffset, GCM_LOCATION_RSX);
			if (g_uDoAlphaTest)   rsxSetFragmentProgramParameter(context, g_fpo, g_uDoAlphaTest, &doATest, fpOffset, GCM_LOCATION_RSX);
			if (g_uOutputScale)   rsxSetFragmentProgramParameter(context, g_fpo, g_uOutputScale, outputScale, fpOffset, GCM_LOCATION_RSX);
			TIMER_END("FLUSH_FP_PATCHES");
			activeFpSlot->generation = g_fullFpGeneration;
		}

		if (patchFragmentProgram ||
		    g_loadedFragmentProgram != FP_FULL ||
		    g_loadedFragmentOffset != fpOffset) {
			rsxLoadFragmentProgramLocation(context, g_fpo, fpOffset,
			                               GCM_LOCATION_RSX);
			g_loadedFragmentProgram = FP_FULL;
			g_loadedFragmentOffset = fpOffset;
		}
	}

	if (g_dirtyBits & DIRTY_FIXED_ENV) {
		/* All-clip-planes-disabled is idempotent; only need to emit once
		 * unless something in setDrawEnv's domain changed. Shares the
		 * DRAW_ENV bit since the trigger conditions overlap heavily. */
		rsxSetUserClipPlaneControl(context,
			GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
			GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
	}

	rsxInvalidateVertexCache(context);
	rsxDrawVertexArray(context, mapPrim(g_primMode), 0, n);

	u32 doneVal = g_vtxLabelNext++;
	if (g_vtxLabelNext == 0) g_vtxLabelNext = 1;
	rsxSetWriteBackendLabel(context, PS3_VTX_LABEL_IDX, doneVal);
	if (++g_unflushedDraws >= PS3_SUBMIT_BATCH_DRAWS)
		submitPendingRsxCommands();
	if (ringSlot >= 0)
		g_vtxRing[ringSlot].labelVal = doneVal;
	else if (oversizeRingSlot >= 0)
		g_vtxOversizeRing[oversizeRingSlot].labelVal = doneVal;
	if (activeFpSlot)
		activeFpSlot->labelVal = doneVal;

	/* State uploaded this flush is now the RSX's current state; clears
	 * let subsequent mutators re-arm only what they actually change. */
	g_dirtyBits = 0;

	TIMER_END("GL_FLUSH");
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

/* Tux is assembled from the same small set of unit-sphere tessellations every
 * frame. Cache their position/normal/UV streams so each body part only copies
 * and colours vertices instead of recalculating trigonometry. The model-view
 * matrix still supplies each node's scale and pose.
 *
 * The public shim accepts up to 24 slices and 16 stacks, whose connected-strip
 * representation tops out at 830 vertices. Tux itself currently uses six
 * tessellations, so eight lazy cache slots leave room without permanent
 * worst-case storage for every possible slices/stacks pair. */
#define PS3_GLU_SPHERE_CACHE_SLOTS 8
#define PS3_GLU_SPHERE_MAX_VERTS   830
struct SphereTemplateVtx {
	float x, y, z;
	float nx, ny, nz;
	float u, v;
};
struct SphereTemplate {
	GLint slices;
	GLint stacks;
	GLint count;
	SphereTemplateVtx *vertices;
};
static SphereTemplate g_sphereTemplates[PS3_GLU_SPHERE_CACHE_SLOTS];
static SphereTemplateVtx g_sphereScratch[PS3_GLU_SPHERE_MAX_VERTS];

static void appendSphereTemplateVertex(SphereTemplateVtx *out, GLint *count,
                                       float x, float y, float z,
                                       float u, float v) {
	if (*count >= PS3_GLU_SPHERE_MAX_VERTS) return;
	SphereTemplateVtx &dst = out[(*count)++];
	dst.x = x; dst.y = y; dst.z = z;
	dst.nx = x; dst.ny = y; dst.nz = z;
	dst.u = u; dst.v = v;
}

static GLint buildSphereTemplate(GLint slices, GLint stacks,
                                 SphereTemplateVtx *out) {
	const float dTheta = 2.f * 3.14159265f / (float)slices;
	const float dPhi   = 3.14159265f / (float)stacks;
	GLint count = 0;
	SphereTemplateVtx previousLast = {};

	for (GLint i = 0; i < stacks; ++i) {
		const float phi0 = (float)i * dPhi;
		const float phi1 = (float)(i + 1) * dPhi;
		const float y0 = cosf(phi0), r0 = sinf(phi0);
		const float y1 = cosf(phi1), r1 = sinf(phi1);

		if (i > 0) {
			/* Preserve the original strip's two-vertex degenerate restart. */
			if (count < PS3_GLU_SPHERE_MAX_VERTS)
				out[count++] = previousLast;
			appendSphereTemplateVertex(out, &count, r1, y1, 0.f,
			                           0.f, (float)(i + 1) / (float)stacks);
		}

		for (GLint j = 0; j <= slices; ++j) {
			const float th = (float)j * dTheta;
			const float ct = cosf(th), st = sinf(th);
			const float nx1 = r1 * ct, ny1 = y1, nz1 = r1 * st;
			appendSphereTemplateVertex(out, &count, nx1, ny1, nz1,
			                           (float)j / (float)slices,
			                           (float)(i + 1) / (float)stacks);

			const float nx0 = r0 * ct, ny0 = y0, nz0 = r0 * st;
			appendSphereTemplateVertex(out, &count, nx0, ny0, nz0,
			                           (float)j / (float)slices,
			                           (float)i / (float)stacks);
			if (j == slices)
				previousLast = out[count - 1];
		}
	}
	return count;
}

static const SphereTemplateVtx *getSphereTemplate(GLint slices, GLint stacks,
                                                   GLint *count) {
	for (GLint i = 0; i < PS3_GLU_SPHERE_CACHE_SLOTS; ++i) {
		const SphereTemplate &entry = g_sphereTemplates[i];
		if (entry.vertices && entry.slices == slices && entry.stacks == stacks) {
			*count = entry.count;
			return entry.vertices;
		}
	}

	*count = buildSphereTemplate(slices, stacks, g_sphereScratch);
	for (GLint i = 0; i < PS3_GLU_SPHERE_CACHE_SLOTS; ++i) {
		SphereTemplate &entry = g_sphereTemplates[i];
		if (entry.vertices) continue;
		entry.vertices = (SphereTemplateVtx *)malloc(
		    (size_t)*count * sizeof(SphereTemplateVtx));
		if (!entry.vertices) break;
		memcpy(entry.vertices, g_sphereScratch,
		       (size_t)*count * sizeof(SphereTemplateVtx));
		entry.slices = slices;
		entry.stacks = stacks;
		entry.count = *count;
		return entry.vertices;
	}

	/* Allocation failure or an unexpected ninth tessellation remains correct;
	 * only that call falls back to the freshly generated scratch stream. */
	return g_sphereScratch;
}

void gluSphere(GLUquadricObj*, GLdouble radius, GLint slices, GLint stacks) {
	if (slices < 3) slices = 3;
	if (stacks < 2) stacks = 2;
	/* Cap tessellation so a single sphere fits the imm buffer. */
	if (slices > 24) slices = 24;
	if (stacks > 16) stacks = 16;

	const float R = (float)radius;

	/* Submit the whole sphere as one connected strip.  The old implementation
	 * called glEnd() once per latitude band, so every visible Tux body part
	 * generated `stacks` RSX draws.  Degenerate vertices join adjacent bands
	 * without producing visible triangles and preserve the winding because
	 * every band contains an even number of vertices. */
	GLint vertexCount = 0;
	const SphereTemplateVtx *vertices =
	    getSphereTemplate(slices, stacks, &vertexCount);
	glBegin(GL_TRIANGLE_STRIP);
	for (GLint i = 0; i < vertexCount && g_immCount < PS3_IMM_MAX; ++i) {
		const SphereTemplateVtx &src = vertices[i];
		ImmVtx &dst = g_immVtx[g_immCount++];
		dst.x = R * src.x; dst.y = R * src.y; dst.z = R * src.z;
		dst.nx = src.nx; dst.ny = src.ny; dst.nz = src.nz;
		dst.u = src.u; dst.v = src.v;
		dst.r = g_color[0]; dst.g = g_color[1];
		dst.b = g_color[2]; dst.a = g_color[3];
	}
	if (vertexCount > 0) {
		const SphereTemplateVtx &last = vertices[vertexCount - 1];
		g_curNx = last.nx; g_curNy = last.ny; g_curNz = last.nz;
		g_curU = last.u; g_curV = last.v;
	}
	glEnd();
}
