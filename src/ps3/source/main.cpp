#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ppu-types.h>
#include <sys/process.h>
#include <sys/tty.h>
#include <io/pad.h>
#include <rsx/rsx.h>
#include <sysutil/sysutil.h>

#include "rsxutil.h"
#include "mesh.h"
#include "mathutils.h"
#include "texture_data.h"

static inline void ttyTrace(const char *msg) {
	u32 w = 0;
	sysTtyWrite(0, msg, __builtin_strlen(msg), &w);
}

SYS_PROCESS_PARAM(1001, 0x100000);

u32 running = 0;

u32 fp_offset;
u32 *fp_buffer;
u32 *texture_buffer;
u32 texture_offset;

rsxProgramConst *projMatrix;
rsxProgramConst *mvMatrix;
rsxProgramAttrib *textureUnit;
rsxProgramConst *eyePosition;
rsxProgramConst *globalAmbient;
rsxProgramConst *litPosition;
rsxProgramConst *litColor;
rsxProgramConst *Kd;
rsxProgramConst *Ks;
rsxProgramConst *spec;

Point3 eye_pos = Point3(0.0f, 5.0f, 15.0f);
Point3 eye_target = Point3(0.0f, 0.0f, 0.0f);
Vector3 up_vec = Vector3(0.0f, 1.0f, 0.0f);

void *vp_ucode = NULL;
rsxVertexProgram *vpo = NULL;
void *fp_ucode = NULL;
rsxFragmentProgram *fpo = NULL;

static Matrix4 P;
static SMeshBuffer *sphere = NULL;
static SMeshBuffer *ground = NULL;

extern "C" {
static void program_exit_callback()
{
	gcmSetWaitFlip(context);
	rsxFinish(context, 1);
}

static void sysutil_exit_callback(u64 status, u64 param, void *usrdata)
{
	switch (status) {
		case SYSUTIL_EXIT_GAME: running = 0; break;
		case SYSUTIL_DRAW_BEGIN:
		case SYSUTIL_DRAW_END:
		default: break;
	}
}
}

static void init_texture()
{
	u32 i;
	u8 *buffer;
	const u8 *data = checkerboard.pixel_data;

	ttyTrace("[trace] init_texture: enter\n");
	texture_buffer = (u32*)rsxMemalign(128, checkerboard.width * checkerboard.height * 4);
	if (!texture_buffer) {
		ttyTrace("[trace] init_texture: rsxMemalign FAILED\n");
		return;
	}

	rsxAddressToOffset(texture_buffer, &texture_offset);

	buffer = (u8*)texture_buffer;
	for (i = 0; i < checkerboard.width * checkerboard.height * 4; i += 4) {
		buffer[i + 0] = *data++;
		buffer[i + 1] = *data++;
		buffer[i + 2] = *data++;
		buffer[i + 3] = *data++;
	}
	ttyTrace("[trace] init_texture: done\n");
}

static void setTexture(u8 texUnit)
{
	gcmTexture texture;

	if (!texture_buffer) return;

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
	texture.width     = checkerboard.width;
	texture.height    = checkerboard.height;
	texture.depth     = 1;
	texture.location  = GCM_LOCATION_RSX;
	texture.pitch     = checkerboard.width * 4;
	texture.offset    = texture_offset;

	rsxLoadTexture(context, texUnit, &texture);
	rsxTextureControl(context, texUnit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(context, texUnit, 0, GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(context, texUnit, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

static void setDrawEnv()
{
	rsxSetColorMask(context, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(context, 0);

	u16 x = 0, y = 0;
	u16 w = display_width, h = display_height;
	float min = 0.0f, max = 1.0f;
	float scale[4], offset[4];

	scale[0] = w * 0.5f;
	scale[1] = h * -0.5f;
	scale[2] = (max - min) * 0.5f;
	scale[3] = 0.0f;
	offset[0] = x + w * 0.5f;
	offset[1] = y + h * 0.5f;
	offset[2] = (max + min) * 0.5f;
	offset[3] = 0.0f;

	rsxSetViewport(context, x, y, w, h, min, max, scale, offset);
	rsxSetScissor(context, x, y, w, h);

	rsxSetDepthTestEnable(context, GCM_TRUE);
	rsxSetDepthFunc(context, GCM_LESS);
	rsxSetShadeModel(context, GCM_SHADE_MODEL_SMOOTH);
	rsxSetDepthWriteEnable(context, 1);
	rsxSetFrontFace(context, GCM_FRONTFACE_CCW);
}

extern "C" {
extern const u8 etr_vp_vpo[];
extern const u8 etr_fp_fpo[];
}

void init_shader()
{
	u32 fpsize = 0, vpsize = 0;

	ttyTrace("[trace] init_shader: loading vertex program\n");
	vpo = (rsxVertexProgram*)etr_vp_vpo;
	rsxVertexProgramGetUCode(vpo, &vp_ucode, &vpsize);

	projMatrix = rsxVertexProgramGetConst(vpo, "projMatrix");
	mvMatrix = rsxVertexProgramGetConst(vpo, "modelViewMatrix");

	if (!projMatrix) ttyTrace("[trace] init_shader: projMatrix is NULL!\n");
	if (!mvMatrix) ttyTrace("[trace] init_shader: mvMatrix is NULL!\n");

	ttyTrace("[trace] init_shader: loading fragment program\n");
	fpo = (rsxFragmentProgram*)etr_fp_fpo;
	rsxFragmentProgramGetUCode(fpo, &fp_ucode, &fpsize);

	fp_buffer = (u32*)rsxMemalign(64, fpsize);
	memcpy(fp_buffer, fp_ucode, fpsize);
	rsxAddressToOffset(fp_buffer, &fp_offset);

	textureUnit   = rsxFragmentProgramGetAttrib(fpo, "texture");
	eyePosition   = rsxFragmentProgramGetConst(fpo, "eyePosition");
	globalAmbient = rsxFragmentProgramGetConst(fpo, "globalAmbient");
	litPosition   = rsxFragmentProgramGetConst(fpo, "lightPosition");
	litColor      = rsxFragmentProgramGetConst(fpo, "lightColor");
	spec          = rsxFragmentProgramGetConst(fpo, "shininess");
	Ks            = rsxFragmentProgramGetConst(fpo, "Ks");
	Kd            = rsxFragmentProgramGetConst(fpo, "Kd");

	if (!textureUnit) ttyTrace("[trace] init_shader: textureUnit is NULL!\n");
	if (!eyePosition) ttyTrace("[trace] init_shader: eyePosition is NULL!\n");
	if (!globalAmbient) ttyTrace("[trace] init_shader: globalAmbient is NULL!\n");
	if (!litPosition) ttyTrace("[trace] init_shader: litPosition is NULL!\n");
	if (!litColor) ttyTrace("[trace] init_shader: litColor is NULL!\n");
	if (!spec) ttyTrace("[trace] init_shader: spec is NULL!\n");
	if (!Ks) ttyTrace("[trace] init_shader: Ks is NULL!\n");
	if (!Kd) ttyTrace("[trace] init_shader: Kd is NULL!\n");

	ttyTrace("[trace] init_shader: done\n");
}

static void drawMesh(SMeshBuffer *mesh, const Matrix4& modelMatrix)
{
	u32 offset;
	Matrix4 viewMatrix = Matrix4::lookAt(eye_pos, eye_target, up_vec);
	Matrix4 modelViewMatrix = transpose(viewMatrix * modelMatrix);

	Matrix4 modelMatrixIT = inverse(modelMatrix);
	Vector4 objEyePos = modelMatrixIT * eye_pos;
	Vector4 objLightPos = modelMatrixIT * Point3(5.0f, 10.0f, 5.0f);

	float globalAmbientColor[3] = {0.15f, 0.15f, 0.15f};
	float lightColorVal[3] = {0.9f, 0.9f, 0.9f};
	float materialColorDiffuse[3] = {0.8f, 0.8f, 0.8f};
	float materialColorSpecular[3] = {0.3f, 0.3f, 0.3f};
	float shininessVal = 20.0f;

	rsxAddressToOffset(&mesh->vertices[0].pos, &offset);
	rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0, offset, sizeof(S3DVertex), 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

	rsxAddressToOffset(&mesh->vertices[0].nrm, &offset);
	rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_NORMAL, 0, offset, sizeof(S3DVertex), 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

	rsxAddressToOffset(&mesh->vertices[0].u, &offset);
	rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0, offset, sizeof(S3DVertex), 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

	rsxLoadVertexProgram(context, vpo, vp_ucode);
	rsxSetVertexProgramParameter(context, vpo, projMatrix, (float*)&P);
	rsxSetVertexProgramParameter(context, vpo, mvMatrix, (float*)&modelViewMatrix);

	float eyePosArr[3] = {objEyePos.getX(), objEyePos.getY(), objEyePos.getZ()};
	float lightPosArr[3] = {objLightPos.getX(), objLightPos.getY(), objLightPos.getZ()};

	rsxSetFragmentProgramParameter(context, fpo, eyePosition, eyePosArr, fp_offset, GCM_LOCATION_RSX);
	rsxSetFragmentProgramParameter(context, fpo, globalAmbient, globalAmbientColor, fp_offset, GCM_LOCATION_RSX);
	rsxSetFragmentProgramParameter(context, fpo, litPosition, lightPosArr, fp_offset, GCM_LOCATION_RSX);
	rsxSetFragmentProgramParameter(context, fpo, litColor, lightColorVal, fp_offset, GCM_LOCATION_RSX);
	rsxSetFragmentProgramParameter(context, fpo, spec, &shininessVal, fp_offset, GCM_LOCATION_RSX);
	rsxSetFragmentProgramParameter(context, fpo, Kd, materialColorDiffuse, fp_offset, GCM_LOCATION_RSX);
	rsxSetFragmentProgramParameter(context, fpo, Ks, materialColorSpecular, fp_offset, GCM_LOCATION_RSX);

	rsxLoadFragmentProgramLocation(context, fpo, fp_offset, GCM_LOCATION_RSX);

	rsxSetUserClipPlaneControl(context,
		GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
		GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);

	rsxAddressToOffset(&mesh->indices[0], &offset);
	rsxDrawIndexArray(context, GCM_TYPE_TRIANGLES, offset, mesh->cnt_indices, GCM_INDEX_TYPE_16B, GCM_LOCATION_RSX);
}

void drawFrame()
{
	static float rot = 0.0f;
	static int frameCount = 0;

	setDrawEnv();
	setTexture(textureUnit->index);

	rsxSetClearColor(context, 0);
	rsxSetClearDepthStencil(context, 0xffffff00);
	rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A | GCM_CLEAR_S | GCM_CLEAR_Z);

	rsxSetZMinMaxControl(context, 0, 1, 1);
	for (u32 i = 0; i < 8; i++)
		rsxSetViewportClip(context, i, display_width, display_height);

	Matrix4 groundModel = makeIdentity();
	drawMesh(ground, groundModel);

	Matrix4 sphereModel = makeTranslation(0.0f, 2.0f, 0.0f) * makeRotationY(DEGTORAD(rot)) * makeRotationX(DEGTORAD(20.0f));
	drawMesh(sphere, sphereModel);

	rot += 1.5f;
	if (rot >= 360.0f) rot -= 360.0f;

	if (frameCount == 0) {
		ttyTrace("[trace] drawFrame: first frame drawn\n");
	}
	frameCount++;
}

int main(int argc, const char *argv[])
{
	padInfo padinfo;
	padData paddata;
	void *host_addr = memalign(HOST_ADDR_ALIGNMENT, HOSTBUFFER_SIZE);

	ttyTrace("[trace] main: starting\n");
	printf("ETR PS3 Demo started...\n");

	init_screen(host_addr, HOSTBUFFER_SIZE);
	ttyTrace("[trace] main: init_screen done\n");

	ioPadInit(7);
	init_shader();
	init_texture();

	sphere = createSphere(2.0f, 24, 24);
	ground = createQuad(20.0f);

	ttyTrace("[trace] main: meshes created\n");

	atexit(program_exit_callback);
	sysUtilRegisterCallback(0, sysutil_exit_callback, NULL);

	P = transpose(Matrix4::perspective(DEGTORAD(45.0f), aspect_ratio, 1.0f, 3000.0f));
	ttyTrace("[trace] main: perspective matrix set\n");

	setDrawEnv();
	setRenderTarget(curr_fb);

	running = 1;
	ttyTrace("[trace] main: entering main loop\n");
	while (running) {
		sysUtilCheckCallback();

		ioPadGetInfo(&padinfo);
		for (int i = 0; i < MAX_PADS; i++) {
			if (padinfo.status[i]) {
				ioPadGetData(i, &paddata);
				if (paddata.BTN_CROSS) goto done;
				if (paddata.ANA_L_H < 100) {
					eye_pos = Point3(eye_pos.getX() - 0.3f, eye_pos.getY(), eye_pos.getZ());
				}
				if (paddata.ANA_L_H > 155) {
					eye_pos = Point3(eye_pos.getX() + 0.3f, eye_pos.getY(), eye_pos.getZ());
				}
			}
		}

		drawFrame();
		flip();
	}

done:
	ttyTrace("[trace] main: exiting\n");
	printf("ETR PS3 Demo done.\n");
	program_exit_callback();
	return 0;
}
