# CLAUDE.md — ETR PS3 port

Quick-reference for PS3 graphics work. Build/install/controls are documented
in `README.md`; this file captures the RSX/graphics architecture so the
codebase doesn't need to be rescanned for every change.

## Project

Extreme Tux Racer 0.8.4 ported to PS3 via PSL1GHT. The Linux OpenGL backend
is retained; a GL shim translates the fixed-function subset the game uses
into RSX + Cg programs.

## Toolchain & build

```bash
export PSL1GHT=/usr/local/ps3dev
export PS3DEV=/usr/local/ps3dev
export PATH=$PS3DEV/bin:$PS3DEV/ppu/bin:$PATH

bash src/ps3/tools/build_ps3.sh         # build + package -> etr.pkg
# build ELF/SSELF only, no .pkg (for dev_deploy.sh iteration):
bash src/ps3/tools/build_ps3.sh --no-pkg
# or, manually:
make -C src -f Makefile.ps3             # -> src/etr.elf, src/etr.self
```

- **Use `src/Makefile.ps3`** (game build). `src/ps3/Makefile` builds the
  standalone `etr_ps3_demo` and is not the game.
- Outputs: `src/etr.elf`, `src/etr.self`, `etr.pkg` (Title ID `EXTR00001`).
- `build_ps3.sh clean` purges `ps3_build/` and stale `src/*.o` (native x86
  build artifacts collide with the PS3 VPATH lookup otherwise).
- The PS3 backend has an explicit source list (`PS3_CPP` in `Makefile.ps3`);
  new `.cpp` files under `src/ps3/source/` are NOT auto-discovered — add them
  to `PS3_CPP`.

## Fast dev iteration (real hardware)

When only code changes (no asset updates), skip the `.pkg` rebuild + reinstall
and hotswap `EBOOT.BIN` over FTP:

```bash
src/ps3/tools/dev_deploy.sh    # build -> sign -> upload EBOOT.BIN via FTP
src/ps3/tools/dev_launch.sh    # mount + auto-play installed ETR via webMAN
src/ps3/tools/dev_close.sh     # close running game -> XMB, block until done
src/ps3/tools/dev_screenshots.sh  # fetch DEMO_MODE PNGs to /tmp, print paths
src/ps3/tools/dev_perflog.sh   # fetch etr_perf.log to stdout
src/ps3/tools/dev_perf_analyze.py  # summarize etr_perf.log (per-tag cost, FPS, spikes)
```

- `dev_deploy.sh` recompiles, then `ppu-strip`+`sprxlinker`+
  `make_self_npdrm` (same content ID as the installed pkg), then uploads
  to `/dev_hdd0/game/EXTR00001/USRDIR/EBOOT.BIN`. Re-launch the game from
  XMB to pick up the new binary.
- `dev_perflog.sh` fetches the runtime perf log (`etr_perf.log`, written to
  the same USRDIR by the perf-timer instrumentation). Print it to a file
  with `> etr_perf.log` or pipe through `tail`/`grep`.
- `dev_perf_analyze.py` parses that log into a per-tag breakdown: phase
  detection (menu vs race), per-frame us and % of frame budget, σ/min/max
  to surface spike-prone paths, and overall FPS. Pipe the log in via
  `dev_perflog.sh > etr_perf.log` then run `dev_perf_analyze.py [etr_perf.log]`.
  Use this instead of eyeballing the raw CSV when comparing runs.
- Defaults target FTP host `192.168.1.245`, user `anonymous`. Override via
  env vars: `PS3_FTP_HOST`, `PS3_FTP_USER`, `PS3_FTP_PASS`,
  `PS3_INSTALL_DIR`.

## Backend-swap convention

Platform-neutral contracts live in `src/n_window.h`, `src/n_audio.h`, etc.
The Linux backends (`src/n_window.cpp`, `src/n_audio.cpp`) are filtered out
of the PS3 build and replaced by `src/ps3/source/n_*_ps3.cpp`. When adding a
new platform-neutral interface, follow the same pattern (header in `src/`,
two implementations, selected by the Makefile).

## PS3 graphics architecture (layered)

```
game code (src/*.cpp, OpenGL fixed-function calls)
        │
        ▼  GL headers under src/ps3/include/GL/ are searched FIRST
GL shim ─────────────  src/ps3/source/ps3_gl.cpp
  matrix stacks, immediate mode, client arrays, textures, blend/depth/alpha,
  lighting, fog, texgen, gluSphere. Emulates the FF subset the game uses.
        │
        ▼  librsx calls
RSX util ────────────  src/ps3/source/rsxutil.cpp
  init_screen(), setRenderTarget(), flip(), vsync, surface config.
        │
        ▼
RSX hardware
```

### Shader pipeline

- Cg sources in `src/ps3/shaders/`, compiled to `.vpo`/`.fpo` via `bin2o`
  and embedded into the ELF.
- **Active shaders** (named in `Makefile.ps3` `VCGFILES`/`FCGFILES`):
  - `etr3d.{vcg,fcg}` — full 3D path (projMatrix/modelViewMatrix, lighting,
    fog, alpha-test, texgen, materials).
  - `etr_ui.fcg` — UI fast path (sampler × vertex colour, no lighting).
- `etr2d.*`, `etr_vp.*`, `etr_fp.*` are legacy/unused.
- Fragment programs are patched per-draw via `rsxSetFragmentProgramParameter`
  (InlineTransfer into an RSX-local ucode copy).

### Draw data flow

1. Game calls `glBegin/glVertex/.../glEnd` or `glDrawArrays/glDrawElements`.
2. Shim flattens into `g_immVtx[]` (`ImmVtx` = 12 floats / 48 bytes:
   `POS(3) NRM(3) TEX0(2) COL(4)` — layout-order matches the GCM attrib
   binds; static_asserts in `ps3_gl.cpp` lock this).
3. `ps3_gl_flush()` (called from `glEnd` and at end of each DrawArrays batch):
   - Acquires the next vertex-ring slot AND the matching fragment-program-ring
     slot (32 slots each, in lockstep — see `PS3_VTX_RING`).
   - `memcpy`s the vertices into the RSX-local ring slot.
   - Binds POS/NRM/TEX0/COL0 attribs, loads vertex program, transposes &
     uploads matrices, patches fragment uniforms, loads fragment program,
     `rsxDrawVertexArray`.
4. Ring slot reuse is fenced by backend label **253**
   (`PS3_VTX_LABEL_IDX`). Label **255** is reserved by rsxutil for flip/idle.

### Fragment-program ring

Each ring slot has its own RSX-local copy of the etr3d ucode; per-draw
constants are patched into the acquired slot. Single-buffering this caused
torn uniforms (draw N+1 overwriting constants while draw N still read them).
The UI program is immutable and has a single permanent copy.

### Matrix convention

Stored column-major (OpenGL). Uploaded **TRANSPOSED** via
`matTranspose()` to match PSL1GHT's Vectormath / cgcomp layout.

### Texture format

- Game-side RGBA8; shim swizzles to **A8R8G8B8** (RSX native) on upload.
- Linear (`GCM_TEXTURE_FORMAT_LIN`), pitch padded to 64 bytes,
  `rsxMemalign(128, ...)`.
- 1×1 white fallback (`g_whiteBuf`) for untextured/unwrap draws.
- Texture cache invalidated before each `rsxLoadTexture`.

### Surface / display

- `setRenderTarget()` in `rsxutil.cpp`: `GCM_SURFACE_X8R8G8B8` colour,
  `GCM_SURFACE_ZETA_Z24S8` depth, double-buffered (`FRAME_BUFFER_COUNT=2`),
  vsync flip.
- Resolution preference is **SD-first** (`480`/`576` before `720`/`1080`)
  for real-hardware performance — see `sResolutionIds` in `rsxutil.cpp`.

## Key files

| Path | Role |
|---|---|
| `src/Makefile.ps3` | Game PS3 build (ppu_rules) |
| `src/ps3/source/ps3_gl.cpp` | GL fixed-function shim (the bulk of the port) |
| `src/ps3/source/rsxutil.cpp` | RSX init, surface config, flip |
| `src/ps3/source/n_window_ps3.cpp` | `RenderWindow` + DualShock pad backend |
| `src/ps3/source/n_audio_ps3.cpp` | Stubbed audio backend |
| `src/ps3/source/ps3_main.cpp` | EBOOT entry, game worker thread |
| `src/ps3/include/rsxutil.h` | Externs for `context`, fb state, pitches |
| `src/ps3/include/ps3_gl_internal.h` | `ps3_gl_init()` entry |
| `src/ps3/include/ps3_tty.h` | `sysTtyTrace` / `sysTtyWriteStr` helpers |
| `src/ps3/include/GL/{gl,glu,glx}.h` | GL shim API the game calls |
| `src/ps3/shaders/etr3d.{vcg,fcg}`, `etr_ui.fcg` | Active Cg shaders |
| `src/ps3/tools/build_ps3.sh` | One-shot build + pkg wrapper |
| `src/ps3/tools/dev_deploy.sh` | FTP hot-deploy: build + sign + upload EBOOT.BIN |
| `src/ps3/tools/dev_perflog.sh` | FTP fetch `etr_perf.log` to stdout |
| `src/ps3/tools/dev_perf_analyze.py` | Summarize `etr_perf.log`: per-tag cost, FPS, spikes |

## Key constants (`ps3_gl.cpp`)

| Constant | Value | Meaning |
|---|---|---|
| `PS3_IMM_MAX` | 4096 | Max verts accumulated before flush |
| `PS3_VTX_RING` | 32 | Vertex + fragment-program ring depth |
| `PS3_VTX_SLOT_MAX` | 512 | Verts per ring slot (oversize path above this) |
| `PS3_VTX_LABEL_IDX` | 253 | Backend label fencing the rings |
| `PS3_MAX_TEXTURES` | 512 | Texture ID pool size |
| `PS3_NUM_LIGHTS` | 4 | Light state slots |

rsxutil labels: `GCM_LABEL_INDEX=255` (flip/idle), `PS3_VTX_LABEL_IDX=253`
(per-draw ring fence).

## Debugging

- **TTY is the only visible log on real hardware.** Use
  `sysTtyTrace("[etr] ...")` (defined in `src/ps3/include/ps3_tty.h`).
  RPCS3 shows these in its log pane; real iron needs a TTY capture.
- Existing trace prefix convention: `[etr] `.
- For richer diagnostics in graphics paths, prefer macros that route through
  `sysTtyWriteStr` rather than `printf`/`assert` (PS3 stderr is not attached).

## Real hardware vs RPCS3

RPCS3 papers over most RSX contract violations; real hardware does not.
Suspect these first when something renders in RPCS3 but is broken or slow on
iron:

- **Alignment** — vertex buffers and surface offsets need 64-byte alignment
  (allocs via `rsxMemalign(64, ...)`); texture base offsets need 128-byte.
  Surface pitches must be 64-aligned. Fragment-program buffers 64-aligned.
- **Coherency** — host-written data the RSX reads needs `asm sync` +
  `rsxInvalidateTextureCache` / `rsxInvalidateVertexCache` before the draw.
- **Primitive counts** — incomplete primitives (`GL_QUADS` with non-multiple-
  of-4 count, etc.) read uninitialized ring bytes and render stray geometry.
- **Stride / attrib offset** — F32 attribs need 4-byte-aligned offsets;
  mismatched stride vs struct layout produces "blown up" triangles.
- **Per-draw uniform patching** — fragment constants patched into a buffer
  still in use by an in-flight draw produce torn floats (red tints, white
  sparkles, scrambled textures). The fragment-program ring exists for this
  reason.
