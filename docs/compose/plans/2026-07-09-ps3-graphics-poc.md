# PS3 Graphics PoC Implementation Plan

> [!NOTE]
> This document may not reflect the current implementation.
> See the final report for up-to-date state:
> [Final Report](../reports/ps3-graphics-poc.md)

> **For agentic workers:** REQUIRED SUB-SKILL: Use compose:subagent (recommended) or compose:execute to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Standalone PS3 demo proving PSL1GHT RSX can render a textured, lit 3D scene — validating the pipeline for an ETR graphics port.

**Architecture:** Single demo app in `src/ps3/` with rsxutil (screen init, flip), mathutils (Matrix4), shaders (Blinn-Phong), and a main loop that renders a lit textured sphere + ground plane with gamepad camera control. Pattern follows `/usr/local/ps3dev/psl1ght/samples/graphics/rsxtest/`.

**Tech Stack:** PSL1GHT RSX, cgcomp shaders (.vcg/.fcg), Vectormath library, PS3 gamepad (ioPad)

---

### Task 1: Create directory structure and Makefile

**Covers:** S5

**Files:**
- Create: `src/ps3/Makefile`
- Create: `src/ps3/source/` directory
- Create: `src/ps3/include/` directory
- Create: `src/ps3/shaders/` directory

- [ ] **Step 1: Create directories**

```bash
mkdir -p src/ps3/source src/ps3/include src/ps3/shaders
```

- [ ] **Step 2: Write Makefile**

Create `src/ps3/Makefile` modeled on `/usr/local/ps3dev/psl1ght/samples/graphics/rsxtest/Makefile`. Key differences: no debugfont_renderer dependency, TARGET=etr_ps3_demo.

```makefile
.SUFFIXES:
ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

include $(PSL1GHT)/ppu_rules

TARGET		:=	etr_ps3_demo
BUILD		:=	build
SOURCES		:=	source
SHADERS		:=	shaders
INCLUDES	:=	include

TITLE		:=	ETR PS3 Demo
APPID		:=	ETR00001
CONTENTID	:=	UP0001-$(APPID)_00-0000000000000000

CFLAGS		=	-Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
CXXFLAGS	=	$(CFLAGS)
LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map
LIBS		:=	-lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm
LIBDIRS		:=

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(SHADERS),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
export BUILDDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
VCGFILES	:=	$(foreach dir,$(SHADERS),$(notdir $(wildcard $(dir)/*.vcg)))
FCGFILES	:=	$(foreach dir,$(SHADERS),$(notdir $(wildcard $(dir)/*.fcg)))

VPOFILES	:=	$(VCGFILES:.vcg=.vpo)
FPOFILES	:=	$(FCGFILES:.fcg=.fpo)

ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES	:=	$(addsuffix .o,$(VPOFILES)) \
					$(addsuffix .o,$(FPOFILES)) \
					$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
					$(sFILES:.s=.o) $(SFILES:.S=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					$(LIBPSL1GHT_INC) \
					-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
					$(LIBPSL1GHT_LIB)

export OUTPUT	:=	$(CURDIR)/$(TARGET)
.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self

run:
	ps3load $(OUTPUT).self

else

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:	$(OFILES)

%.vpo.o	:	%.vpo
	@echo $(notdir $<)
	@$(bin2o)

%.fpo.o	:	%.fpo
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
```

- [ ] **Step 3: Verify Makefile parses**

Run: `make -n` from `src/ps3/`
Expected: "Nothing to be done" (no sources yet, but no parse errors)

---

### Task 2: Create math utilities header

**Covers:** S4

**Files:**
- Create: `src/ps3/include/mathutils.h`

PSL1GHT provides `<vectormath/cpp/vectormath_aos.h>` with Vector3, Vector4, Matrix4. We need helper functions: `perspective()`, `lookAt()`, `rotationX()`, `rotationY()`, `rotationZ()`, `translation()`, `inverse()`, `transpose()`. The rsxtest already uses these — copy the pattern from `/usr/local/ps3dev/psl1ght/samples/graphics/rsxtest/include/mesh.h` (which uses Vectormath::Aos).

- [ ] **Step 1: Write mathutils.h**

Create `src/ps3/include/mathutils.h`:

```cpp
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
```

- [ ] **Step 2: Verify header compiles**

Run: `echo '#include "mathutils.h"' | ppu-g++ -c -x c++ - -Iinclude -I/usr/local/ps3dev/ppu/include` from `src/ps3/`
Expected: no errors

---

### Task 3: Create mesh header

**Covers:** S3

**Files:**
- Create: `src/ps3/include/mesh.h`

Copy the S3DVertex / SMeshBuffer pattern from rsxtest. Adds helper functions to create sphere and quad meshes with RSX-aligned memory.

- [ ] **Step 1: Write mesh.h**

Create `src/ps3/include/mesh.h`:

```cpp
#ifndef __MESH_H__
#define __MESH_H__

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <vectormath/cpp/vectormath_aos.h>

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
```

---

### Task 4: Create rsxutil (screen init and flip)

**Covers:** S3

**Files:**
- Create: `src/ps3/include/rsxutil.h`
- Create: `src/ps3/source/rsxutil.cpp`

Copy from `/usr/local/ps3dev/psl1ght/samples/graphics/rsxtest/source/rsxutil.cpp` and `/usr/local/ps3dev/psl1ght/samples/graphics/rsxtest/include/rsxutil.h`. Remove DebugFont dependency (we'll add it later if needed).

- [ ] **Step 1: Write rsxutil.h**

```cpp
#ifndef __RSXUTIL_H__
#define __RSXUTIL_H__

#include <ppu-types.h>
#include <rsx/rsx.h>

#define DEFAULT_CB_SIZE      0x80000
#define HOST_ADDR_ALIGNMENT  (1024*1024)
#define HOSTBUFFER_SIZE      (128*1024*1024)
#define FRAME_BUFFER_COUNT   2

extern gcmContextData *context;
extern u32 curr_fb;
extern u32 display_width;
extern u32 display_height;
extern u32 depth_pitch;
extern u32 depth_offset;
extern u32 *depth_buffer;
extern u32 color_pitch;
extern u32 color_offset[FRAME_BUFFER_COUNT];
extern u32 *color_buffer[FRAME_BUFFER_COUNT];
extern float aspect_ratio;

void setRenderTarget(u32 index);
void init_screen(void *host_addr, u32 size);
void waitflip();
void flip();

#endif
```

- [ ] **Step 2: Write rsxutil.cpp**

Copy from rsxtest's rsxutil.cpp, removing DebugFont references. The core logic is: `initVideoConfiguration()` (resolution detection), `init_screen()` (RSX init + buffer allocation), `setRenderTarget()` (gcmSurface setup), `flip()` (double-buffer swap).

```cpp
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ppu-types.h>
#include <sysutil/video.h>
#include "rsxutil.h"

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

static u32 sResolutionIds[] = {
	VIDEO_RESOLUTION_960x1080,
	VIDEO_RESOLUTION_720,
	VIDEO_RESOLUTION_480,
	VIDEO_RESOLUTION_576
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
}

void setRenderTarget(u32 index)
{
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

	for (u32 i = 0; i < FRAME_BUFFER_COUNT; i++) {
		color_buffer[i] = (u32*)rsxMemalign(64, display_height * color_pitch);
		rsxAddressToOffset(color_buffer[i], &color_offset[i]);
		gcmSetDisplayBuffer(i, color_offset[i], color_pitch, display_width, display_height);
	}

	depth_buffer = (u32*)rsxMemalign(64, display_height * depth_pitch);
	rsxAddressToOffset(depth_buffer, &depth_offset);
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
```

---

### Task 5: Create vertex and fragment shaders

**Covers:** S4

**Files:**
- Create: `src/ps3/shaders/etr_vp.vcg`
- Create: `src/ps3/shaders/etr_fp.fcg`

- [ ] **Step 1: Write vertex shader**

`src/ps3/shaders/etr_vp.vcg`:

```hlsl
void main
(
	float3 vertexPosition : POSITION,
	float3 vertexNormal : NORMAL,
	float2 vertexTexcoord : TEXCOORD0,

	uniform float4x4 projMatrix,
	uniform float4x4 modelViewMatrix,

	out float4 ePosition : POSITION,
	out float4 oPosition : TEXCOORD0,
	out float3 oNormal : TEXCOORD1,
	out float2 oTexcoord : TEXCOORD2
)
{
	ePosition = mul(mul(projMatrix, modelViewMatrix), float4(vertexPosition, 1.0f));
	oPosition = float4(vertexPosition, 1.0f);
	oNormal = vertexNormal;
	oTexcoord = vertexTexcoord;
}
```

- [ ] **Step 2: Write fragment shader**

`src/ps3/shaders/etr_fp.fcg`:

```hlsl
void main
(
	float4 position : TEXCOORD0,
	float3 normal : TEXCOORD1,
	float2 texcoord : TEXCOORD2,

	uniform float3 globalAmbient,
	uniform float3 lightPosition,
	uniform float3 lightColor,
	uniform float3 eyePosition,
	uniform float3 Kd,
	uniform float3 Ks,
	uniform float shininess,

	uniform sampler2D texture,

	out float4 oColor
)
{
	float3 N = normalize(normal);
	float3 L = normalize(lightPosition - position.xyz);
	float diffuseLight = max(dot(N, L), 0.0f);
	float3 diffuse = Kd * lightColor * diffuseLight;

	float3 V = normalize(eyePosition - position.xyz);
	float3 H = normalize(L + V);
	float specularLight = pow(max(dot(H, N), 0.0f), shininess);
	if (diffuseLight <= 0) specularLight = 0;
	float3 specular = Ks * specularLight;

	float3 color = tex2D(texture, texcoord).xyz * (diffuse + globalAmbient) + specular;
	oColor = float4(color, 1.0f);
}
```

- [ ] **Step 3: Compile shaders**

Run: `cgcomp -pvp src/ps3/shaders/etr_vp.vcg src/ps3/build/etr_vp.vpo` and `cgcomp -pfp src/ps3/shaders/etr_fp.fcg src/ps3/build/etr_fp.fpo`
Expected: compiled .vpo and .fpo files in build/

---

### Task 6: Create embedded texture data

**Covers:** S3

**Files:**
- Create: `src/ps3/include/texture_data.h`

Generate a simple 64x64 checkerboard RGBA texture as a C header (like rsxtest's acid.h). This avoids needing to load files from the PS3 filesystem.

- [ ] **Step 1: Generate checkerboard texture header**

Create `src/ps3/include/texture_data.h` with a 64x64 checkerboard pattern (white/gray squares). Use a script or write directly:

```cpp
#ifndef __TEXTURE_DATA_H__
#define __TEXTURE_DATA_H__

static const struct {
	unsigned int width;
	unsigned int height;
	unsigned int bytes_per_pixel;
	unsigned char pixel_data[64 * 64 * 4];
} checkerboard = {
	64, 64, 4,
	// Generated: alternating 8x8 white (#FFFFFF) and light gray (#C0C0C0) squares
	// Each pixel is 4 bytes: R, G, B, A
};
```

The actual pixel data will be generated programmatically in the script.

---

### Task 7: Create main.cpp with rendering loop

**Covers:** S2, S3, S4

**Files:**
- Create: `src/ps3/source/main.cpp`

This is the core demo: init RSX, compile shaders, create meshes, set up texture, render loop with gamepad camera.

- [ ] **Step 1: Write main.cpp**

`src/ps3/source/main.cpp`:

```cpp
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ppu-types.h>
#include <sys/process.h>
#include <io/pad.h>
#include <rsx/rsx.h>
#include <sysutil/sysutil.h>

#include "rsxutil.h"
#include "mesh.h"
#include "mathutils.h"
#include "texture_data.h"

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

	texture_buffer = (u32*)rsxMemalign(128, checkerboard.width * checkerboard.height * 4);
	if (!texture_buffer) return;

	rsxAddressToOffset(texture_buffer, &texture_offset);

	buffer = (u8*)texture_buffer;
	for (i = 0; i < checkerboard.width * checkerboard.height * 4; i += 4) {
		buffer[i + 0] = *data++; // R
		buffer[i + 1] = *data++; // G
		buffer[i + 2] = *data++; // B
		buffer[i + 3] = *data++; // A
	}
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

// Shader binary data (will be generated by cgcomp and linked via bin2o)
extern "C" {
extern const u8 etr_vp[];
extern const u8 etr_fp[];
}

void init_shader()
{
	u32 fpsize = 0, vpsize = 0;

	vpo = (rsxVertexProgram*)etr_vp;
	rsxVertexProgramGetUCode(vpo, &vp_ucode, &vpsize);

	projMatrix = rsxVertexProgramGetConst(vpo, "projMatrix");
	mvMatrix = rsxVertexProgramGetConst(vpo, "modelViewMatrix");

	fpo = (rsxFragmentProgram*)etr_fp;
	rsxFragmentProgramGetUCode(fpo, &fp_ucode, &fpsize);

	fp_buffer = (u32*)rsxMemalign(64, fpsize);
	memcpy(fp_buffer, fp_ucode, fpsize);
	rsxAddressToOffset(fp_buffer, &fp_offset);

	textureUnit  = rsxFragmentProgramGetAttrib(fpo, "texture");
	eyePosition  = rsxFragmentProgramGetConst(fpo, "eyePosition");
	globalAmbient = rsxFragmentProgramGetConst(fpo, "globalAmbient");
	litPosition  = rsxFragmentProgramGetConst(fpo, "lightPosition");
	litColor     = rsxFragmentProgramGetConst(fpo, "lightColor");
	spec         = rsxFragmentProgramGetConst(fpo, "shininess");
	Ks           = rsxFragmentProgramGetConst(fpo, "Ks");
	Kd           = rsxFragmentProgramGetConst(fpo, "Kd");
}

static void drawMesh(SMeshBuffer *mesh, const Matrix4& modelMatrix)
{
	u32 offset;
	Matrix4 modelMatrixIT = inverse(modelMatrix);
	Matrix4 modelViewMatrix = transpose(lookAt(eye_pos, eye_target, up_vec) * modelMatrix);

	Vector4 objEyePos = modelMatrixIT * Vector4(eye_pos.getX(), eye_pos.getY(), eye_pos.getZ(), 1);
	Vector4 objLightPos = modelMatrixIT * Vector4(5.0f, 10.0f, 5.0f, 1);

	float globalAmbientColor[3] = {0.15f, 0.15f, 0.15f};
	float lightColorVal[3] = {0.9f, 0.9f, 0.9f};
	float materialColorDiffuse[3] = {0.8f, 0.8f, 0.8f};
	float materialColorSpecular[3] = {0.3f, 0.3f, 0.3f};
	float shininess = 20.0f;

	rsxAddressToOffset(&mesh->vertices[0].pos, &offset);
	rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0, offset, sizeof(S3DVertex), 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

	rsxAddressToOffset(&mesh->vertices[0].nrm, &offset);
	rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_NORMAL, 0, offset, sizeof(S3DVertex), 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

	rsxAddressToOffset(&mesh->vertices[0].u, &offset);
	rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0, offset, sizeof(S3DVertex), 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

	rsxLoadVertexProgram(context, vpo, vp_ucode);
	rsxSetVertexProgramParameter(context, vpo, projMatrix, (float*)&P);
	rsxSetVertexProgramParameter(context, vpo, mvMatrix, (float*)&modelViewMatrix);

	rsxSetFragmentProgramParameter(context, fpo, eyePosition, (float*)&objEyePos, fp_offset, GCM_LOCATION_RSX);
	rsxSetFragmentProgramParameter(context, fpo, globalAmbient, globalAmbientColor, fp_offset, GCM_LOCATION_RSX);
	rsxSetFragmentProgramParameter(context, fpo, litPosition, (float*)&objLightPos, fp_offset, GCM_LOCATION_RSX);
	rsxSetFragmentProgramParameter(context, fpo, litColor, lightColorVal, fp_offset, GCM_LOCATION_RSX);
	rsxSetFragmentProgramParameter(context, fpo, spec, &shininess, fp_offset, GCM_LOCATION_RSX);
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

	setDrawEnv();
	setTexture(textureUnit->index);

	rsxSetClearColor(context, 0);
	rsxSetClearDepthStencil(context, 0xffffff00);
	rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A | GCM_CLEAR_S | GCM_CLEAR_Z);

	rsxSetZMinMaxControl(context, 0, 1, 1);
	for (u32 i = 0; i < 8; i++)
		rsxSetViewportClip(context, i, display_width, display_height);

	// Draw ground plane
	Matrix4 groundModel = makeIdentity();
	drawMesh(ground, groundModel);

	// Draw sphere with rotation
	Matrix4 sphereModel = makeTranslation(0.0f, 2.0f, 0.0f) * makeRotationY(DEGTORAD(rot)) * makeRotationX(DEGTORAD(20.0f));
	drawMesh(sphere, sphereModel);

	rot += 1.5f;
	if (rot >= 360.0f) rot -= 360.0f;
}

int main(int argc, const char *argv[])
{
	padInfo padinfo;
	padData paddata;
	void *host_addr = memalign(HOST_ADDR_ALIGNMENT, HOSTBUFFER_SIZE);

	printf("ETR PS3 Demo started...\n");

	init_screen(host_addr, HOSTBUFFER_SIZE);
	ioPadInit(7);
	init_shader();
	init_texture();

	sphere = createSphere(2.0f, 24, 24);
	ground = createQuad(20.0f);

	atexit(program_exit_callback);
	sysUtilRegisterCallback(0, sysutil_exit_callback, NULL);

	P = makePerspective(DEGTORAD(45.0f), aspect_ratio, 0.1f, 100.0f);

	setDrawEnv();
	setRenderTarget(curr_fb);

	running = 1;
	while (running) {
		sysUtilCheckCallback();

		ioPadGetInfo(&padinfo);
		for (int i = 0; i < MAX_PADS; i++) {
			if (padinfo.status[i]) {
				ioPadGetData(i, &paddata);
				if (paddata.BTN_CROSS) goto done;
				// Left stick X rotates camera
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
	printf("ETR PS3 Demo done.\n");
	program_exit_callback();
	return 0;
}
```

---

### Task 8: Build verification

**Covers:** S5, S6

- [ ] **Step 1: Run make**

Run: `cd src/ps3 && make`
Expected: Successful compilation producing `etr_ps3_demo.elf` and `etr_ps3_demo.self`

- [ ] **Step 2: Verify .self exists**

Run: `ls -la src/ps3/etr_ps3_demo.self`
Expected: File exists, non-zero size

- [ ] **Step 3: Verify ELF symbols**

Run: `ppu-nm src/ps3/etr_ps3_demo.elf | grep -E "main|drawFrame|init_shader"`
Expected: Symbol entries for main, drawFrame, init_shader
