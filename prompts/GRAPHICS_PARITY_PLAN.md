# ETR → PS3 Graphics Parity Plan (native librsx renderer)

## Context

The PS3 port has a working proof-of-concept (`src/ps3/`, `etr_ps3_demo.self`) that validates the RSX pipeline: a textured, single-light Blinn-Phong sphere on a ground quad. But ETR's full renderer uses far more — textured quadtree terrain, sphere-skeleton Tux, skybox, fog, particles, track marks, HUD, text — none of which the PoC supports.

**Key discovery:** ETR does **not** use Irrlicht. It uses **SFML 2.4 + fixed-function OpenGL 1.x**, with ~504 `gl*()` calls across 15 files. The entire render-state surface is funneled through one switch — `set_gl_options(mode)` in `src/ogl.cpp:196-396` — wrapped by the `ScopedRenderMode` RAII helper. The per-frame draw order is a clean sequential list (`src/racing.cpp:309-340`).

**Decision (confirmed with user):** Build a **fixed-function-GL emulation layer on librsx** that implements the `gl*` entry points ETR already calls, reusing the PoC's Cg shaders and RSX draw patterns as the kernel. This keeps ETR's game/render source byte-for-byte identical to the known-good Linux build (so any visual bug is attributable to the renderer, not a logic regression), and avoids rewriting 15 files.

## Architecture: FF-GL emulation on librsx (approach A)

Game source sees `<GL/gl.h>` / `<GL/glu.h>` (included via `src/bh.h:29`). On PS3 those resolve to shim headers in `src/ps3/include/gl/`, and the `gl*` symbols come from a new `ps3_gl` static lib linked instead of `-lGL`/`-lGLU`.

**Why this and not a rewrite of ETR's draw code (approach B):**
- `set_gl_options(mode)` (`ogl.cpp:196-396`) is a perfect chokepoint — 10 render modes, each a bulk state toggle that maps directly to `rsxSet*` calls.
- The vertex-array path (terrain) is the bulk of pixels and already maps 1:1 to the PoC's `rsxBindVertexArrayAttrib` + `rsxDrawIndexArray` (`quadtree.cpp:1127-1146` ↔ `src/ps3/source/main.cpp:210-258`).
- Immediate mode (`glBegin/glEnd`) is bounded and small — only the fog-plane strip (`env.cpp:351-373`), Tux's shadow (`tux.cpp:672-728`), and track marks (`track_marks.cpp:117-174`).
- `gluSphere` is the one GLU hard case, used in one place (`tux.cpp:366-373`), and the PoC already has a sphere mesh generator (`src/ps3/include/mesh.h:32`).

Two narrow internal carve-outs (not game-source rewrites): `gluSphere` → cached indexed sphere; `glTexGenfv(GL_OBJECT_LINEAR)` → computed in the vertex shader.

## Module design (all new code under `src/ps3/`)

```
src/ps3/
  include/
    gl/gl.h, gl/glu.h        NEW: FF-GL API surface seen by game (replaces <GL/gl.h>)
    rsxutil.h, mathutils.h   existing — reused (mathutils: add makeOrtho, stack helpers)
    mesh.h                   existing — extend (32-bit indices, dynamic buffers)
    rsx_state.h              NEW: enum→GCM_* translation, render-target/flip helpers
    rsx_texture.h            NEW: PNG→RSX texture cache (depends on SHIM sf::Texture)
    rsx_shader.h             NEW: program handle + uniform upload
    rsx_immediate.h          NEW: glBegin/glEnd accumulator (private to ps3_gl)
    rsx_matrix.h             NEW: modelview/projection/texture matrix stacks
    rsx_ffstate.h            NEW: FF state struct (lights, material, fog, texgen, blend)
  source/
    main.cpp                 existing PoC — stays as regression-test harness
    rsxutil.cpp              existing — reused (bump heap, wire stencil)
    ps3_gl.cpp               NEW: the gl* entry-point surface (core)
    ps3_glu.cpp              NEW: gluPerspective, gluSphere cache, gluErrorString
    rsx_state.cpp            NEW: begin_frame/end_frame (from PoC setDrawEnv)
    rsx_texture.cpp          NEW
    rsx_shader.cpp           NEW
    rsx_immediate.cpp        NEW
    rsx_matrix.cpp           NEW
    rsx_ffstate.cpp          NEW
  shaders/
    etr_vp.vcg, etr_fp.fcg   existing — extend (see below)
```

**Single game-source edit:** `src/bh.h` — header swap to the platform dispatcher (owned by `prompts/SHIM_LAYER_PLAN.md`); on PS3 `<GL/gl.h>` → `src/ps3/include/gl/gl.h`. Optionally undefine `USE_STENCIL_BUFFER` (`bh.h:76`) if the stencil shadow proves flaky (clean alpha-fallback already exists). No other game file is modified.

### Shader changes (extend, don't replace)

One shader pair with uniform-gated flags serves every render mode (avoids 10 variants):
- **VP**: add `COLOR` attrib (color-material), `texGenOn` + S/T plane uniforms for `GL_OBJECT_LINEAR`; output texcoord switches between vertex uv and `dot(pos, plane)`.
- **FP**: add `lightingOn`, `fogOn`+`fogColor`/`fogStart`/`fogEnd`, `colorMaterialOn`, `texture2dOn`, `textureEnvMode` (MODULATE/DECAL/REPLACE), up to 4 lights. Output alpha must be computed (currently hardcoded 1.0 — needed for trees/particles/trackmarks/skybox/HUD). If Cg instruction budget is pressured, split into 2-3 variants keyed by mode (cheap `rsxLoad*Program` cost only).

## Phased plan (each phase = verifiable on-screen milestone)

**Phase 1 — Static textured lit terrain from a real course (e.g. Bunny Hill).**
Wire `CCourse::Load`, `InitQuadtree`, `RenderCourse`/`RenderQuadtree`, `setup_course_tex_gen`, `setup_view_matrix`, `set_gl_options(COURSE)`.
Implement in `ps3_gl`: matrix stack (MV+PROJ), `gluPerspective`, `glViewport`, vertex arrays (`glVertexPointer`/`glNormalPointer`/`glColorPointer`), `glDrawElements` with **32-bit indices**, texgen (shader), COLOR_MATERIAL, LIGHT0 + `glMaterialfv`, `TTexture::Bind`→sampler, depth LEQUAL + cull.
*First thing to validate before any other Phase-1 work:* **32-bit RSX indices** (`GCM_INDEX_TYPE_32B`) — the quadtree uses `GLuint*` indices (`quadtree.cpp:993`) and a 256×256 heightmap = 65,536 verts, right at the 16-bit ceiling.
*Milestone:* Bunny Hill terrain mesh, textured by terrain type, lit, correct perspective, fixed camera; compare side-by-side with Linux build.

**Phase 2 — Skybox + linear fog + fog plane.**
Wire `Env.DrawSkybox`, `SetupFog`, `DrawFog`; `set_gl_options(SKY/FOG_PLANE)`.
Add: immediate-mode `glBegin(GL_QUAD_STRIP)` (fog plane — first use of `rsx_immediate`), `glFogfv`→shader uniforms, `glDrawArrays(GL_TRIANGLE_FAN)` fan→tri decomposition (skybox), `GL_DECAL` texenv, depth-mask-off for sky.
*Milestone:* sky surrounds camera, distant terrain fades to fog color, ground-fog band visible.

**Phase 3 — Tux + shadow.**
Wire `CCharShape::Draw`/`DrawNodes`/`DrawCharSphere`, `DrawShadow`+`TraverseDagForShadow`; `set_gl_options(TUX/TUX_SHADOW)`.
Add: `gluSphere` via cached indexed spheres (one per `divisions`), deep MV push/pop (recursive DAG), per-node `glMaterialfv`, `GL_NORMALIZE`, stencil state (`glStencilFunc/glStencilOp`). Depth format is already `GCM_SURFACE_ZETA_Z24S8` so stencil bits exist.
*Milestone:* recognizable multi-material Tux at start position; shadow on terrain. If stencil misbehaves, undefine `USE_STENCIL_BUFFER` in `bh.h` — alpha-blob fallback needs no other change.

**Phase 4 — Particles, track marks, trees, items, snow.**
Wire `draw_particles`, `DrawTrackmarks`, `DrawTrees`+items, 3D `DrawSnow`/curtains; `set_gl_options(PARTICLES/TREES/TRACK_MARKS)`.
Add: `GL_ALPHA_TEST` + `glAlphaFunc(GL_GEQUAL,0.5)`, `glDrawArrays(GL_QUADS/QUAD_STRIP)` decomposition, heavier immediate-mode (track marks), depth-write-off for track marks.
*Defer* GUI 2D snow (`sf::Sprite`) to Phase 5.
*Milestone:* snow spray behind Tux, crisp alpha-edged trees, track marks, falling snow.

**Phase 5 — HUD, text, fonts, GUI (full playable loop).**
Wire `DrawHud`, `CFont::DrawString`, `DrawGUI`/`DrawFrameX`; `set_gl_options(GUI/GAUGE_BARS/TEXFONT)`; `Winsys.beginSFML/endSFML`.
Add: `glOrtho`, gauge-bar VAAs + 2D texgen, 2D fast path for `sf::Text`/`sf::Sprite`/`sf::RectangleShape`.
*This phase fully merges with SHIM_LAYER_PLAN items 5-7 (Font/Text/Sprite/RenderWindow).*
*Milestone:* boot → menus → race → results, fully playable.

## SHIM dependency (`prompts/SHIM_LAYER_PLAN.md`)

Graphics parity can proceed **independently through Phase 3** — only `sf::Image`/`sf::Texture` (PNG decode) + `sf::Texture::bind` hook are needed as a Phase-1 prerequisite. Phase 4 is independent (defers GUI snow). Phase 5 converges with the SHIM Font/Text/Sprite/RenderWindow work.

**Recommended sequencing:** build the SHIM `sf::Image`/`sf::Texture`/`bind` first (Phase 1 prereq), then run graphics Phases 1-4 alongside the SHIM font/text work; converge at Phase 5. The PS3 `sf::Texture` should store decoded RGBA in main memory and lazily upload to RSX on first `bind()`, caching the RSX offset — making `TTexture::Bind` (`textures.cpp:52-54`) the single point where RSX sampler state is set.

## Risks (priority order)

1. **32-bit RSX indices (HIGH)** — mandatory for terrain (`quadtree.cpp:993` GLuint). Validate in PSL1GHT **first**, before any other Phase-1 work. If only 16-bit works, terrain must be tiled into ≤64K-vertex chunks (much larger change).
2. **1 MiB heap (HIGH)** — PoC `SYS_PROCESS_PARAM(1001, 0x100000)` (`main.cpp:24`). Bump to ~64 MiB (`0x4000000`); PS3 has ~220-240 MiB usable XDR.
3. **Stencil shadow (MED)** — has clean `USE_STENCIL_BUFFER`-off fallback (one-line `bh.h` toggle).
4. **Immediate-mode decomposition (MED)** — bounded; 4096-vert scratch buffer is ample (largest draw is the ~8-vert fog strip). Must honor `glColor`/`glNormal`/`glTexCoord` set mid-primitive.
5. **RSX memory (LOW)** — 316 PNGs worst-case ~80 MiB of 256 MiB GDDR; terrain VNC ~2.4 MiB/course. Add LRU eviction if needed.
6. **Fog plane vs linear fog (LOW)** — both needed; they compose (linear fog = shader; fog plane = geometry in FOG_PLANE mode).
7. **Shader instruction budget (LOW)** — prefer uniform flags over branches; split into 2-3 variants if needed.

## Verification

- **Per phase:** the milestone listed above, viewed on PS3 hardware or RPCS3, compared against the Linux build at the same camera pose.
- **Regression canary:** the existing PoC `etr_ps3_demo.self` keeps building after each shader/RSX change — confirms the underlying pipeline still works.
- **Build:** `cd src/ps3 && make clean && make` exits 0 and produces `etr_ps3_demo.self`; a new top-level PS3 target builds the full game (`make -f Makefile.ps3` or equivalent) linking `ps3_gl` instead of `-lGL`/`-lGLU`, plus the SFML shim instead of SFML.
- **Sanity:** ELF symbols for the full game contain `main`, `CRacing::Loop`, `RenderQuadtree`, `CCharShape::Draw`, `DrawHud`, and the `gl*` surface resolving to `ps3_gl.cpp` definitions (not external `-lGL`).

## Open items to confirm before Phase 1

- 32-bit RSX indexing works in this PSL1GHT build (blocks Phase 1 — test first).
- Target resolution (720p vs 1080p — affects fragment load on the fog/texgen shader).
- Test environment: real PS3 hardware vs RPCS3 only (RPCS3 has edge cases around stencil and 32-bit indices).
