# ETR PS3 Port — Phase 1 Plan: Real Terrain on librsx

## Context

The PS3 port has a working proof-of-concept (`src/ps3/`, `etr_ps3_demo.self`) that draws a textured, lit Blinn-Phong sphere on a ground quad — but shares **no** code with the actual game and uses 16-bit indices, a 1 MiB heap, and a vertex format with no color attribute. Phase 1 of `prompts/GRAPHICS_PARITY_PLAN.md` is the first on-screen milestone that renders ETR's *real* renderer output: the quadtree terrain of a course (Bunny Hill), textured by terrain type, lit, correct perspective, fixed camera — side-by-side comparable with the Linux build.

The chosen architecture is a **fixed-function-GL emulation layer on librsx**: the game source keeps calling `gl*()` unchanged; on PS3 those resolve to shim headers backed by a new `ps3_gl` static lib that translates GL state into RSX commands and one uber-shader. This keeps ETR's game/render logic byte-identical to the known-good Linux build (so any visual bug is attributable to the renderer, not a logic regression).

**Scope decisions (confirmed):**
- The plan is **self-contained** — it delivers the minimal `sf::Image` / `sf::Texture` / `sf::Texture::bind` shim needed to reach the milestone (handoff to `SHIM_LAYER_PLAN.md` is only at Font/Text/Sprite/RenderWindow, Phase 5).
- **Both real PS3 hardware and RPCS3 are available.** The 32-bit-index spike (T0) and milestone are verified on real HW as source of truth; RPCS3 is used for fast iteration.
- **Target resolution: 720p** (PoC default); one-line change if 1080p is wanted later.

## Verified facts driving the design

| Fact | Source |
|---|---|
| PoC heap = 1 MiB, indices = `u16` (`GCM_INDEX_TYPE_16B`), `S3DVertex` = pos+nrm+uv (no color) | `src/ps3/source/main.cpp:24,226-257`, `src/ps3/include/mesh.h:11-20,26` |
| PoC texture path: `gcmTexture` A8R8G8B8\|LIN + RGBA→ARGB shuffle, setTexture at `main.cpp:100-131` | `src/ps3/source/main.cpp:75-131` |
| Game terrain VNC: stride **36** (`STRIDE_GL_ARRAY = 8*GLfloat + 4*GLubyte`), pos@0, nrm@16, color(4×UBYTE)@32 | `src/course.h:28`, `src/quadtree.cpp:1130-1139` |
| Game indices: `GLuint*` = **32-bit**, `new GLuint[6*RowSize*NumRows]`; Bunny Hill 90×260 → ≤140,400 indices | `src/quadtree.cpp:993`, `src/quadtree.h:77` |
| VNC array = `new GLubyte[]` in main XDR (not RSX memory) | `src/course.cpp:249` |
| `set_gl_options(COURSE)` state block (TEXTURE_2D/DEPTH_TEST/CULL_FACE/LIGHTING/BLEND/TEXTURE_GEN_S+T/COLOR_MATERIAL on; DEPTH_LEQUAL; depthmask TRUE; smooth; OBJECT_LINEAR texgen; no `glColorMaterial` call → default AMBIENT_AND_DIFFUSE tracking) | `src/ogl.cpp:243-261` |
| Texgen planes: S={1/6,0,0,0}, T={0,0,1/6,0} (S=x/6, T=z/6) | `src/course_render.cpp:35-40` |
| `glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, VertexArrayIndices)` per terrain-type pass, preceded by `Course.TerrList[j].texture->Bind()` | `src/quadtree.cpp:735-736,750-758` |
| `TTexture::Bind` → `sf::Texture::bind(&texture)` — sole texture bind chokepoint; `TTexture::Load` → `sf::Texture::loadFromFile` + setSmooth(true) + setRepeated(repeatable=true for terrain) | `src/textures.cpp:42-54` |
| Matrix: NO push/pop in terrain path. `view.cpp:141` does `ctrl->view_mat.GetTransposed()` then `glLoadMatrix(view_mat)` — so the shim receives a **column-major** matrix (GL spec convention) | `src/view.cpp:141,156` |
| `TMatrix<4,4>` internal layout is `_data[row][col]` (row-major) | `src/matrices.h:24,29` |
| PoC uploads `transpose(viewMatrix*modelMatrix)` to VP → Cg/RSX `float4x4` is **row-major in upload memory** | `src/ps3/source/main.cpp:214,236-237` |
| Per-frame prefix: ClearRenderContext → SetupFog → update_view → SetupLight → RenderCourse → Reshape → SwapBuffers | `src/racing.cpp:304-345` |

**Matrix-layout consequence (the easiest bug to introduce):** store GL matrices column-major (pure GL semantics); transpose **once** at the RSX upload boundary. Do not transpose on `glLoadMatrixd`.

## Approach (recommended)

1. **T0 blocker spike** — prove 32-bit RSX indices work in this PSL1GHT build on real HW *before* any other work.
2. Build a minimal `platform.h` dispatcher + PS3 `sf::Image`/`sf::Texture` shim (stb_image decode, lazy RSX upload on first `bind`, RGBA→ARGB shuffle) + `gl/gl.h`/`gl/glu.h` shim headers, and swap `src/bh.h` SFML includes to the dispatcher.
3. Build `ps3_gl` core: matrix stacks (MV depth 32 / PROJ depth 2), FF-state struct, dirty-flag flush, `gl*Pointer`/`glDrawElements` with **zero-copy XDR reads** via `GCM_LOCATION_CELL`.
4. Extend the Cg uber-shader with COLOR attrib, object-linear texgen, and uniform flags (lightingOn/colorMaterialOn/texture2dOn/textureEnvMode).
5. Wire the real game course-load + quadtree render path through the shim via a purpose-built minimal PS3 `main` (not `CRacing::Loop`, which pulls in Tux/particles/etc.).

## Task breakdown (each task has an on-screen verification milestone)

### T0 — 32-bit RSX index spike (BLOCKER; do first, on real HW)
- Edit `src/ps3/include/mesh.h`: add `createBigGrid(u32 side)` generating a 300×300 (=90,000 verts, >65,535) grid; extend `SMeshBuffer` with `u32 *indices32` + `index_stride`; allocate indices32 via `rsxMemalign`.
- Edit `src/ps3/source/main.cpp`: bump `SYS_PROCESS_PARAM` heap to 4 MiB; add `drawMesh32` (clone of `drawMesh` using `GCM_INDEX_TYPE_32B`); create + draw the big grid in `drawFrame()` with a uv-checkerboard so vertex>65535 sampling is visible.
- **Verify:** grid renders fully on real HW with no RSX fault; sphere+ground regression canary still works.
- **If it fails on HW:** stop. Terrain must be tiled into ≤64K-vertex chunks — major re-plan, out of Phase-1 scope.

### T1 — Platform dispatcher + minimal sf:: shim + gl shim headers (compiles, no draw)
- New `src/platform.h` (6-line conditional: `__PS3__` → `platform_ps3.h`, else → `platform_sfml.h`).
- New `src/platform_sfml.h` = the existing SFML includes moved out of `bh.h`.
- New `src/platform_ps3.h` (Phase-1 cut): real `sf::Image`, `sf::Texture`, `sf::Color`, `sf::Vector2<T>`; **forward-decl stubs** for Font/Text/Sprite/RenderWindow/Clock/Event/Joystick/Sound (satisfy includes, never instantiated at Phase 1).
- New `src/ps3/include/stb_image.h` (upstream single-header; `#define STB_IMAGE_IMPLEMENTATION` in `rsx_texture.cpp`; `#define STBI_NO_THREAD_LOCALS`).
- New `src/ps3/include/gl/gl.h` + `gl/glu.h`: GL types/constants + `gl*`/`glu*` declarations matching `<GL/gl.h>` surface. Add `-Isrc/ps3/include` so `#include <GL/gl.h>` resolves naturally (zero edits to game source for the GL include).
- Edit `src/bh.h:29-32`: replace SFML `#include`s with `#include "platform.h"`.
- **Verify:** Linux build still compiles (`make`, no `__PS3__`); a trivial ppu cpp including `platform_ps3.h` + `gl/gl.h` compiles.

### T2 — ps3_gl skeleton + matrix stack + FFState (draws a clear screen)
- New `src/ps3/include/rsx_matrix.h` + `source/rsx_matrix.cpp`: `MatrixStack { float elements[DEPTH][16]; int top; }` (column-major); `mvStack` (depth 32), `projStack` (depth 2); impl of `glMatrixMode/glLoadIdentity/glLoadMatrixd/glPushMatrix/glPopMatrix/glMultMatrixd/glTranslatef/glRotatef`.
- New `src/ps3/include/rsx_ffstate.h` + `source/rsx_ffstate.cpp`: `FFState` struct (caps, depthFunc/Mask, cullFace, shadeModel, texgen mode+planes, texEnvMode, `FFLight lights[4]`, `FFMaterial`, clearColor/stencil, `VertexArrayBinding vertexAttribs[4]`, `boundTexture`, `dirtyMask`).
- New `src/ps3/include/rsx_state.h` + `source/rsx_state.cpp`: `begin_frame/end_frame` lifted from PoC `setDrawEnv` (`main.cpp:133-160`) + `flip` (from `rsxutil.cpp:158-170`).
- New `src/ps3/source/ps3_gl.cpp`: initial entry points only — `glClearColor`, `glClear` (eager → `rsxSetClearColor`/`rsxClearSurface`), `glViewport` (eager → `rsxSetViewport`), matrix ops, `glMatrixMode`.
- New `src/ps3/source/ps3_glu.cpp`: `gluPerspective` (build projection via `mathutils.h::makePerspective`), `gluErrorString`.
- New `src/ps3/source/ps3_game_main.cpp` (`SYS_PROCESS_PARAM(1001, 0x4000000)` = 64 MiB): init RSX (reuse PoC `init_screen`/`init_shader`), loop calling `glClear` + `flip` only.
- **Verify:** solid clear-color screen on HW — first confirmation the new game-target build runs.

### T3 — First triangle via glDrawElements (unlit, 16-bit, XDR)
- Extend `ps3_gl.cpp`: `glEnableClientState`/`glDisableClientState`, `glVertexPointer` (records into `FFState.vertexAttribs[]`), `glDrawElements` — flush walks enabled attribs, calls `rsxBindVertexArrayAttrib(..., GCM_LOCATION_CELL)` with `rsxAddressToOffset(pointer)`, then `rsxDrawIndexArray(..., GCM_LOCATION_CELL)`.
- Use the existing PoC shader verbatim with manual MVP upload.
- Hardcode a 3-vert triangle in a `new float[]` (XDR) + `u16` indices.
- **Verify:** single triangle at fixed camera. Confirms XDR-direct vertex reads.

### T4 — 32-bit indices through the shim
- Add `GL_UNSIGNED_INT → GCM_INDEX_TYPE_32B` in `glDrawElements`. Replace the T3 triangle with the T0 big-grid geometry driven via `glVertexPointer` + `glDrawElements`.
- **Verify:** full big grid renders through the FF-GL path. Second confirmation of T0, now via the shim.

### T5 — sf::Texture shim + flushTexture (textured grid)
- Complete `sf::Texture` (in `platform_ps3.h`/`source/rsx_texture.cpp`): `loadFromFile` → stb decode RGBA → `flipVertically` (PNG top-left → GL bottom-left) → stage; `setSmooth`/`setRepeated` stored; `bind(const Texture*)` → lazy RSX upload on first bind (`rsxMemalign` + RGBA→ARGB shuffle per `main.cpp:75-98`, cache `rsxOffset_`), then record into `FFState.boundTexture` + set `DIRTY_TEXTURE`.
- Implement `flushTexture()` in `ps3_gl.cpp` (build `gcmTexture` A8R8G8B8|LIN, `rsxLoadTexture`/`rsxTextureControl`/`rsxTextureFilter`/`rsxTextureWrapMode` mirroring PoC `setTexture`, with smooth→LINEAR / repeated→WRAP). Wire into the draw flush sequence. Free stb staging after upload.
- Bind a real ETR terrain PNG (e.g. `data/terrains/snow.png`) via `sf::Texture::loadFromFile`.
- **Verify:** textured grid sampling the terrain PNG correctly (orientation, color, repeat).

### T6 — Extended uber-shader (texgen + color + flags)
- Rewrite `src/ps3/shaders/etr_vp.vcg`: add `COLOR` attrib input; add uniforms `texGenOn`, `texGenPlaneS`, `texGenPlaneT`; output `oColor` (TEXCOORD3); `oTexcoord = texGenOn ? (dot(float4(pos,1),planeS), dot(float4(pos,1),planeT)) : vertexTexcoord`. (The homogeneous `1.0f` in the dot product is mandatory — without it S=x/6 breaks.)
- Rewrite `src/ps3/shaders/etr_fp.fcg`: add `lightingOn`, `colorMaterialOn` (material diffuse ← interpolated `vColor`), `texture2dOn`, `textureEnvMode` (0=MODULATE/1=DECAL/2=REPLACE/3=ADD); compute output alpha (existing PoC hardcodes 1.0). Keep Blinn-Phong core. For Phase 1, lights passed in **world space** (terrain modelview = view only, so object==world; revisit inverse-model for Phase 3 Tux).
- Update `init_shader` to fetch new VP/FP uniform handles.
- **Verify:** re-run T5 with `texGenOn=true` + S/T planes {1/6,0,0,0}/{0,0,1/6,0}; texture now tiles every 6 units (was stretched across the grid).

### T7 — Course load + quadtree init (no render)
- New `src/Makefile.ps3`: compile game subset — `course.cpp, course_render.cpp, quadtree.cpp, ogl.cpp, textures.cpp, env.cpp, view.cpp, matrices.cpp, vectors.cpp, bh.cpp, mathlib.cpp, spx.cpp, common.cpp, game_config.cpp` — plus all ps3_gl/shim sources; link `-lsimdmath -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm`.
- New `src/ps3/source/ps3_stubs.cpp`: no-op/assert stubs for Phase 3-5 symbols (`tux.cpp, particles.cpp, track_marks.cpp, snow, hud.cpp, font.cpp, gui.cpp, winsys.cpp, audio.cpp, states.cpp, keyframe.cpp, character.cpp, loop.cpp`) — derive exact symbol list from `nm` on the Linux build.
- In `ps3_game_main.cpp`: set `param.data_dir` to the PS3 absolute course path; call `Course.LoadCourseList()` + `Course.LoadCourse(bunny_hill)` + `InitQuadtree()`. Render still disabled.
- **Verify:** TTY log: course parsed, N terrain types loaded, VNC array populated (dump first/last vert). No crash.
- **Pitfall:** `game_config.cpp` may init audio paths — stub/strip. Confirm `param.data_dir` resolves `data/courses/default/bunny_hill/`.

### T8 — RenderCourse via the shim (THE MILESTONE)
- In `ps3_game_main.cpp`: set fixed camera (translate+Z view via `glLoadMatrixd`), then per frame: `ClearRenderContext()`, `Env.SetupLight()` (Phase-1 cut: just `LIGHT0` from `default_light`), `set_gl_options(COURSE)` (drives FFState caps), `setup_course_tex_gen()` (drives texgen planes), `set_material(white, black, 1.0)`, `RenderCourse()`, `Reshape(1280,720)`, `flip()`.
- Full dirty-flag flush in `glDrawElements`: matrix → caps (`rsxSetDepthTestEnable`/`rsxSetDepthFunc` LEQUAL/`rsxSetDepthWriteEnable`/`rsxSetCullFaceEnable`+`rsxSetCullFace` BACK/`rsxSetShadeModel` SMOOTH/`rsxSetBlendEnable`) → texture → material (`Kd`/`Ks`/`shininess`) → lights → texgen → texenv; defensively `rsxInvalidateTextureCache`; draw with `GCM_INDEX_TYPE_32B` + `GCM_LOCATION_CELL`.
- **Verify:** **Bunny Hill terrain, textured by terrain type, lit, correct perspective, fixed camera** — side-by-side with Linux build at the same camera pose.

### T9 — Multi-texture correctness (post-milestone polish)
- Confirm each terrain-type pass rebinds its texture (the lazy flush must set `DIRTY_TEXTURE` on every `sf::Texture::bind`, since `Course.TerrList[j].texture->Bind()` changes between draws at `quadtree.cpp:750-758`).
- Confirm no texture bleeding between passes.

## Key design decisions

- **Vertex/index placement: zero-copy XDR.** Read the game's `new GLubyte[]` VNC array and `new GLuint[]` index array directly via `GCM_LOCATION_CELL` (`rsxBindVertexArrayAttrib`, `rsxDrawIndexArray`). No per-frame copy. RSX-local mirroring deferred to a later optimization phase.
- **No new PS3 vertex struct.** `gl*Pointer(size,type,stride,pointer)` carries everything `rsxBindVertexArrayAttrib` needs. Color via `GCM_VERTEX_DATA_TYPE_U8F` (normalized [0,1]), `size=4`. Verify normalization with a known-gray vertex; fallback: float4 + `/255` in VP.
- **One uber-shader with uniform flags** (matches parent plan; RSX FP limit 512 instr, this is ~30). Split into lit/unlit variants only if branch cost shows up.
- **Lazy/dirty-flag state flush** before each `glDrawElements`; `glClear`/`glViewport` eager. GL caps → RSX calls for test/blend/cull/depth; LIGHTING/TEXTURE_2D/COLOR_MATERIAL/TEXGEN become shader uniforms.

## Critical files

**New (Phase 1):** `src/platform.h`, `src/platform_sfml.h`, `src/platform_ps3.h`, `src/ps3/include/stb_image.h`, `src/ps3/include/gl/gl.h`, `src/ps3/include/gl/glu.h`, `src/ps3/include/rsx_matrix.h`, `src/ps3/include/rsx_ffstate.h`, `src/ps3/include/rsx_state.h`, `src/ps3/source/rsx_matrix.cpp`, `src/ps3/source/rsx_ffstate.cpp`, `src/ps3/source/rsx_state.cpp`, `src/ps3/source/ps3_gl.cpp`, `src/ps3/source/ps3_glu.cpp`, `src/ps3/source/rsx_texture.cpp`, `src/ps3/source/ps3_game_main.cpp`, `src/ps3/source/ps3_stubs.cpp`, `src/Makefile.ps3`.

**Modified:** `src/bh.h` (SFML includes → `platform.h`), `src/ps3/shaders/etr_vp.vcg` + `etr_fp.fcg` (T6), `src/ps3/source/main.cpp` + `src/ps3/include/mesh.h` (T0 spike + heap bump).

**Untouched (regression canary):** `src/ps3/Makefile` and the existing PoC build path — `etr_ps3_demo.self` must keep building after every change.

**Reuse from PoC:** `init_screen`, `setRenderTarget`, `flip` (`src/ps3/source/rsxutil.cpp`); `makePerspective`/`makeLookAt` (`src/ps3/include/mathutils.h`); the texture-load sequence at `src/ps3/source/main.cpp:75-131`; the attrib-bind + draw pattern at `src/ps3/source/main.cpp:226-257`.

## Risks (priority)

1. **HIGH — 32-bit indices in PSL1GHT:** T0 validates on real HW first. Failure → terrain tiling re-plan (out of scope).
2. **HIGH — Matrix transpose confusion:** three layouts (TMatrix row-major, GL column-major, Cg row-major). Test with a simple translated camera before lighting.
3. **MED — Heap pressure:** 64 MiB + free stb staging after upload; watch for null `malloc` if many terrain textures stay decoded.
4. **MED — Color attrib normalization:** confirm `U8F` normalizes; fallback float4 + `/255`.
5. **MED — Per-draw command overhead** from XDR-direct reads with many small quadtree passes; Phase-1 fixed camera tolerates; revisit Phase 4.
6. **LOW — RPCS3 quirks:** real HW is source of truth; treat RPCS3 as a fast sanity check.

## Verification

- **Per task:** the on-screen milestone listed above, viewed on real PS3 hardware (RPCS3 for quick iteration), compared against the Linux build at the matching camera pose once T8 is reached.
- **Regression canary:** after every change, `cd src/ps3 && make clean && make` exits 0 and `etr_ps3_demo.self` still renders the sphere+ground.
- **Phase-1 build:** `make -f Makefile.ps3` (from `src/`) exits 0 and produces the PS3 game `.self`; ELF symbols resolve `main`, `RenderCourse`/`RenderQuadtree`, and the `gl*` surface to `ps3_gl.cpp` definitions (not external `-lGL`).
- **Final milestone (T8):** Bunny Hill terrain matches the Linux build side-by-side.
