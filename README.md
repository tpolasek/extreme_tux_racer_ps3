# Extreme Tux Racer — PS3 (PSL1GHT) Port

Minimum viable PS3 port of Extreme Tux Racer 0.8.4. The 2D menu path renders
via an OpenGL-to-RSX emulation shim; pad input, TTY logging, and asset
packaging are working. Sound is stubbed and the 3D race rendering is deferred
(menu-only scope).

Upstream project: https://sourceforge.net/projects/extremetuxracer/

## Prerequisites

The PSL1GHT/ps3dev toolchain, with portlibs (`freetype`, `libpng`, `zlib`):

```bash
export PSL1GHT=/usr/local/ps3dev
export PS3DEV=/usr/local/ps3dev
export PATH=$PS3DEV/bin:$PS3DEV/ppu/bin:$PATH
```

Verify with `ppu-gcc --version`.

## Build

One command compiles `src/Makefile.ps3` and packages the installable `.pkg`:

```bash
bash src/ps3/tools/build_ps3.sh
```

Options:

| Command | Result |
|---|---|
| `build_ps3.sh` | compile + package (`etr.pkg`) |
| `build_ps3.sh clean` | remove `ps3_build/`, stale `*.o`, and final artifacts |
| `build_ps3.sh -j4` | extra args pass through to `make` |
| `build_ps3.sh --no-pkg` | build `etr.elf`/`etr.self` only |

The script purges stale `src/*.o` before each build — a native (x86-64)
autotools build leaves `.o` files there that the PS3 Makefile's VPATH would
otherwise link by mistake (`File in wrong format` / EM:62).

### Manual steps (if you prefer)

```bash
make -f src/Makefile.ps3          # -> src/etr.elf, src/etr.self
bash src/ps3/tools/make_pkg.sh    # -> etr.pkg
```

## Output

| File | Description |
|---|---|
| `src/etr.elf` | PowerPC64 ELF, statically linked |
| `src/etr.self` | signed PS3 executable |
| `etr.pkg` | installable package: `EBOOT.BIN` + `USRDIR/data/` + icon |

**Title ID:** `EXTR00001` (9 chars — required; shorter IDs corrupt the install
folder name). **Content ID:** `UP0001-EXTR00001_00-0000000000000001`.

## Running

### RPCS3
1. Install `etr.pkg` (File → Install .pkg, or double-click it).
2. Boot **Extreme Tux Racer** (`EXTR00001`).
3. Watch the log (under `~/.config/rpcs3/dev_hdd0/` or the GUI log) for
   `[etr] ...` traces:
   ```
   [etr] EBOOT entry
   [etr] game thread entry
   [etr] InitConfig / InitGame / Winsys.Init
   [etr] RenderWindow::create: init RSX
   [etr] ps3_gl_init: ready
   [etr] LoadTextureList / LoadFontlist / LoadMusicList
   [etr] Run(SplashScreen)
   ```

### Real PS3
Install `etr.pkg` via a package manager, or `ps3load src/etr.self`.

## Port layout

```
src/
  Makefile.ps3              PSL1GHT (ppu_rules) build for the game
  ps3/
    include/GL/{gl,glu,glx}.h   fixed-function GL API the game calls
    include/ps3_gl_internal.h   ps3_gl_init()
    include/ps3_tty.h           sysTtyWrite helper for [etr] traces
    shaders/etr2d.{vcg,fcg}     single 2D Cg program (proj*mv, textured)
    source/
      ps3_gl.cpp            GL emulation: matrix stacks, immediate mode,
                            RGBA->ARGB texture upload, clear; 3D FF no-ops
      n_window_ps3.cpp      RenderWindow + DualShock input backend
      n_audio_ps3.cpp       stubbed audio (MusicStream::open -> true)
      ps3_main.cpp          EBOOT entry, 2 MiB worker thread, heap param
      rsxutil.cpp           RSX init/flip (unchanged from the demo)
    tools/
      build_ps3.sh          one-shot build + package
      make_pkg.sh           strip -> make_self_npdrm -> sfo.py -> pkg.py
```

## Scope

- **Working:** splash screen, menu graphics (2D), DualShock navigation,
  asset packaging, `[etr]` TTY traces, XMB quit handling.
- **Stubbed:** audio (no playback), `clock_gettime` (uses `sysGetCurrentTime`).
- **Deferred:** 3D course/race rendering (lighting/fog/texgen/gluSphere are
  accepted no-ops), sound playback.
