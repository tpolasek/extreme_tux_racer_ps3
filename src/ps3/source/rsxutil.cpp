#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ppu-types.h>
#include <sysutil/video.h>
#include "rsxutil.h"
#include "ps3_gfx_assert.h"

#define GCM_LABEL_INDEX 255

videoResolution vResolution;
gcmContextData *context = NULL;

u32 curr_fb = 0;
u32 first_fb = 1;
u32 display_width;
u32 display_height;
u32 depth_pitch;
u32 depth_offset;
u32 *depth_buffer;
u32 color_pitch;
u32 color_offset[FRAME_BUFFER_COUNT];
u32 *color_buffer[FRAME_BUFFER_COUNT];
float aspect_ratio;

/* Prefer the lowest SD framebuffer for real-hardware performance.  The
 * console/output configuration decides whether the regional 480/576 mode is
 * interlaced or progressive. */
static u32 sResolutionIds[] = {
	VIDEO_RESOLUTION_480,
	VIDEO_RESOLUTION_576,
	VIDEO_RESOLUTION_720,
	VIDEO_RESOLUTION_960x1080,
	VIDEO_RESOLUTION_1080
};
static size_t RESOLUTION_ID_COUNT = sizeof(sResolutionIds) / sizeof(u32);

static u32 sLabelVal = 1;

static void waitFinish()
{
	rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, sLabelVal);
	rsxFlushBuffer(context);
	while (*(volatile u32*)gcmGetLabelAddress(GCM_LABEL_INDEX) != sLabelVal)
		usleep(30);
	++sLabelVal;
}

static void waitRSXIdle()
{
	rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, sLabelVal);
	rsxSetWaitLabel(context, GCM_LABEL_INDEX, sLabelVal);
	++sLabelVal;
	waitFinish();
}

static void initVideoConfiguration()
{
	s32 rval = 0;
	s32 resId = 0;

	for (size_t i = 0; i < RESOLUTION_ID_COUNT; i++) {
		rval = videoGetResolutionAvailability(VIDEO_PRIMARY, sResolutionIds[i], VIDEO_ASPECT_AUTO, 0);
		if (rval != 1) continue;
		resId = sResolutionIds[i];
		rval = videoGetResolution(resId, &vResolution);
		if (!rval) break;
	}

	if (rval) {
		printf("Error: videoGetResolutionAvailability failed.\n");
		exit(1);
	}

	videoConfiguration config = {
		(u8)resId,
		VIDEO_BUFFER_FORMAT_XRGB,
		VIDEO_ASPECT_AUTO,
		{0,0,0,0,0,0,0,0,0},
		(u32)vResolution.width * 4
	};

	rval = videoConfigure(VIDEO_PRIMARY, &config, NULL, 0);
	if (rval) {
		printf("Error: videoConfigure failed.\n");
		exit(1);
	}

	videoState state;
	rval = videoGetState(VIDEO_PRIMARY, 0, &state);
	switch (state.displayMode.aspect) {
		case VIDEO_ASPECT_4_3:  aspect_ratio = 4.0f / 3.0f; break;
		case VIDEO_ASPECT_16_9: aspect_ratio = 16.0f / 9.0f; break;
		default: aspect_ratio = 16.0f / 9.0f; break;
	}

	display_height = vResolution.height;
	display_width = vResolution.width;
	printf("[etr] video mode: %ux%u\n", display_width, display_height);
}

void setRenderTarget(u32 index)
{
	/* Surface offsets and pitches must be 64-aligned on real RSX or
	 * rsxSetSurface corrupts the colour/depth tile config — RPCS3 hides
	 * this; real iron shows up as torn or blown-up geometry. */
	GFX_ASSERT_ALIGNED(color_offset[index], 64);
	GFX_ASSERT_ALIGNED(depth_offset, 64);
	GFX_ASSERT(GFX_IS_MULT(color_pitch, 64), "color_pitch must be 64-aligned");
	GFX_ASSERT(GFX_IS_MULT(depth_pitch, 64), "depth_pitch must be 64-aligned");
	GFX_ASSERT(index < FRAME_BUFFER_COUNT, "render-target index out of range");

	gcmSurface sf;
	sf.colorFormat      = GCM_SURFACE_X8R8G8B8;
	sf.colorTarget      = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0]   = color_offset[index];
	sf.colorPitch[0]    = color_pitch;
	sf.colorLocation[1] = GCM_LOCATION_RSX;
	sf.colorLocation[2] = GCM_LOCATION_RSX;
	sf.colorLocation[3] = GCM_LOCATION_RSX;
	sf.colorOffset[1]   = 0;
	sf.colorOffset[2]   = 0;
	sf.colorOffset[3]   = 0;
	sf.colorPitch[1]    = 64;
	sf.colorPitch[2]    = 64;
	sf.colorPitch[3]    = 64;
	sf.depthFormat      = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation    = GCM_LOCATION_RSX;
	sf.depthOffset      = depth_offset;
	sf.depthPitch       = depth_pitch;
	sf.type             = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias        = GCM_SURFACE_CENTER_1;
	sf.width            = display_width;
	sf.height           = display_height;
	sf.x                = 0;
	sf.y                = 0;
	rsxSetSurface(context, &sf);
}

void init_screen(void *host_addr, u32 size)
{
	u32 zs_depth = 4;
	u32 color_depth = 4;

	rsxInit(&context, DEFAULT_CB_SIZE, size, host_addr);
	initVideoConfiguration();
	waitRSXIdle();
	gcmSetFlipMode(GCM_FLIP_VSYNC);

	color_pitch = display_width * color_depth;
	depth_pitch = display_width * zs_depth;

	/* RSX requires 64-byte aligned surface pitches. SD widths (640/720)
	 * satisfy this trivially but a future 1080 mode (1920*4 = 7680) is
	 * also 64-aligned — catch any mode that breaks the contract. */
	GFX_ASSERT(GFX_IS_MULT(color_pitch, 64), "color_pitch not 64-aligned");
	GFX_ASSERT(GFX_IS_MULT(depth_pitch, 64), "depth_pitch not 64-aligned");
	GFX_ASSERT_ALIGNED(host_addr, HOST_ADDR_ALIGNMENT);

	for (u32 i = 0; i < FRAME_BUFFER_COUNT; i++) {
		color_buffer[i] = (u32*)rsxMemalign(64, display_height * color_pitch);
		GFX_ASSERT(color_buffer[i] != NULL, "color_buffer rsxMemalign failed");
		GFX_ASSERT_ALIGNED(color_buffer[i], 64);
		s32 rc = rsxAddressToOffset(color_buffer[i], &color_offset[i]);
		GFX_ASSERT(rc == 0, "rsxAddressToOffset(color_buffer) failed");
		GFX_ASSERT_ALIGNED(color_offset[i], 64);
		gcmSetDisplayBuffer(i, color_offset[i], color_pitch, display_width, display_height);
	}

	depth_buffer = (u32*)rsxMemalign(64, display_height * depth_pitch);
	GFX_ASSERT(depth_buffer != NULL, "depth_buffer rsxMemalign failed");
	GFX_ASSERT_ALIGNED(depth_buffer, 64);
	s32 rc = rsxAddressToOffset(depth_buffer, &depth_offset);
	GFX_ASSERT(rc == 0, "rsxAddressToOffset(depth_buffer) failed");
	GFX_ASSERT_ALIGNED(depth_offset, 64);
}

void waitflip()
{
	while (gcmGetFlipStatus() != 0)
		usleep(200);
	gcmResetFlipStatus();
}

void flip()
{
	if (!first_fb) waitflip();
	else gcmResetFlipStatus();

	gcmSetFlip(context, curr_fb);
	rsxFlushBuffer(context);
	gcmSetWaitFlip(context);

	curr_fb ^= 1;
	setRenderTarget(curr_fb);
	first_fb = 0;
}
