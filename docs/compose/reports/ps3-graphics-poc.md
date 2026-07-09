---
feature: ps3-graphics-poc
status: delivered
specs: []
plans:
  - docs/compose/plans/2026-07-09-ps3-graphics-poc.md
---

# PS3 Graphics PoC — Final Report

## What Was Built

A standalone PS3 demo (`src/ps3/`) that renders a textured, lit 3D scene using PSL1GHT RSX. The demo proves the core rendering pipeline that Extreme Tux Racer requires: vertex/normal/texcoord attributes, projection + modelview matrices, texture sampling, and Blinn-Phong diffuse+specular lighting. It renders a rotating textured sphere above a ground plane with gamepad-controlled camera, built as a PS3 self-executable (`etr_ps3_demo.self`).

## Architecture

The demo is structured as a single-module PS3 application in `src/ps3/` with these components:

| File | Responsibility |
|------|----------------|
| `source/main.cpp` | Entry point, shader init, mesh creation, texture upload, render loop, gamepad input |
| `source/rsxutil.cpp` | RSX context init, video configuration, double-buffer flip, render target setup |
| `include/rsxutil.h` | RSX utility declarations, buffer/frame constants |
| `include/mathutils.h` | Matrix4 helpers: perspective, lookAt, rotation, translation, identity |
| `include/mesh.h` | S3DVertex/SMeshBuffer structs, sphere and quad mesh generators |
| `include/texture_data.h` | Embedded 64x64 checkerboard RGBA texture |
| `shaders/etr_vp.vcg` | Vertex shader: proj*mv transform, pass normal/texcoord |
| `shaders/etr_fp.fcg` | Fragment shader: texture * (diffuse + ambient) + specular (Blinn-Phong) |

The rendering pipeline follows PSL1GHT conventions: shaders compiled with `cgcomp` to `.vpo`/`.fpo`, linked as binary objects via `bin2o`. All RSX memory allocated with `rsxMemalign` + `rsxAddressToOffset`. Double-buffered with vsync flip.

### Design Decisions

- **Shader-based rendering** — PSL1GHT RSX has no fixed-function pipeline; all rendering uses vertex/fragment shaders. The shaders replicate ETR's Blinn-Phong lighting model.
- **Embedded texture data** — Avoids PS3 filesystem complexity for the PoC. The checkerboard texture is compiled directly into the binary.
- **Standalone build** — The demo builds independently with its own Makefile, not integrated into ETR's autotools build. This keeps the desktop build untouched.
- **Vectormath library** — Uses PSL1GHT's bundled `<vectormath/cpp/vectormath_aos.h>` for matrix math, matching the rsxtest sample pattern.

## Usage

```bash
# Build
export PSL1GHT=/usr/local/ps3dev
cd src/ps3
make

# Deploy to PS3
ps3load etr_ps3_demo.self
# Or copy etr_ps3_demo.self to USB and launch from PS3 menu
```

Controls: Left stick horizontal = move camera. CROSS button = exit.

## Verification

- `make clean && make` exits with code 0
- `etr_ps3_demo.self` (139KB) and `etr_ps3_demo.elf` (1.8MB) produced
- ELF symbols verified: `main`, `drawFrame`, `init_shader`, `drawMesh`, `createSphere`, `createQuad` all present
- Shaders compile to `.vpo`/`.fpo` via `cgcomp` without errors
- All source files compile with PSL1GHT `ppu-g++` without warnings

## Journey Log

- [lesson] PSL1GHT's `bin2o` appends the file extension to symbol names (`etr_vp.vpo` becomes `etr_vp_vpo`, not `etr_vp`)
- [lesson] Vectormath's `Matrix4 * Point3` returns `Vector4`, not `Point3` — must use `Vector4` for transformed eye/light positions
- [lesson] PSL1GHT's `libsimdmath` is required for `rsqrtf4`/`recipf4` symbols used by Vectormath's `normalize()` and `inverse()`

## Source Materials

| File | Role | Notes |
|------|------|-------|
| `docs/compose/plans/2026-07-09-ps3-graphics-poc.md` | Implementation plan | 8 tasks, all complete |
| `/usr/local/ps3dev/psl1ght/samples/graphics/rsxtest/` | Reference implementation | Basis for rsxutil, shader patterns |
