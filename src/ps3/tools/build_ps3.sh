#!/usr/bin/env bash
#
# One-shot ETR PS3 build: compiles src/Makefile.ps3 -> etr.{elf,self}, then
# runs make_pkg.sh to produce the installable etr.pkg.
#
# Usage:
#   src/ps3/tools/build_ps3.sh           # build + package
#   src/ps3/tools/build_ps3.sh clean     # remove build dir + all artifacts
#   src/ps3/tools/build_ps3.sh -j4       # extra args pass through to make
#   src/ps3/tools/build_ps3.sh --no-pkg  # build .elf/.self only, skip packaging
#
# Requires PSL1GHT/PS3DEV in the environment (the ps3dev toolchain).
#
# NOTE: the PS3 Makefile's VPATH includes src/, so any stale .o files left in
# src/ by a native (x86-64) autotools build would be linked in by mistake
# ("File in wrong format" / "EM: 62"). To guarantee the PS3 build is clean we
# remove src/*.o + src/*.o.d before compiling. They're regenerable artifacts,
# so the next native `make` just recompiles them.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SRC="$ROOT/src"

MAKEFILE="Makefile.ps3"
PKG_SCRIPT="$SRC/ps3/tools/make_pkg.sh"

DO_PKG=1
DO_CLEAN=0
MAKE_ARGS=()
for arg in "$@"; do
	case "$arg" in
		clean|--clean) DO_CLEAN=1;;
		--no-pkg)      DO_PKG=0;;
		*)             MAKE_ARGS+=("$arg");;
	esac
done

# Toolchain sanity check — fail early with a clear message.
if [ -z "${PS3DEV:-}" ] || [ -z "${PSL1GHT:-}" ]; then
	echo "ERROR: PS3DEV/PSL1GHT not set." >&2
	echo "  export PSL1GHT=/usr/local/ps3dev" >&2
	echo "  export PS3DEV=/usr/local/ps3dev" >&2
	echo "  export PATH=\$PS3DEV/bin:\$PS3DEV/ppu/bin:\$PATH" >&2
	exit 1
fi
command -v ppu-gcc >/dev/null || { echo "ERROR: ppu-gcc not on PATH." >&2; exit 1; }

cd "$SRC"

# full clean: PS3 build dir + stale native .o files + final artifacts.
if [ "$DO_CLEAN" -eq 1 ]; then
	echo "==> clean"
	make -f "$MAKEFILE" clean 2>/dev/null || true
	rm -f "$SRC"/*.o "$SRC"/*.o.d
	rm -f "$ROOT"/etr.pkg "$SRC"/etr.elf "$SRC"/etr.self "$SRC"/etr.elf.map
	echo "==> clean done"
	exit 0
fi

# Pre-build: purge stale native (x86-64) .o files so VPATH can't pick them up.
if compgen -G "$SRC/*.o" >/dev/null; then
	echo "==> removing stale .o files in src/ (avoids x86/PS3 link conflicts)"
	rm -f "$SRC"/*.o "$SRC"/*.o.d
fi

echo "==> make -f $MAKEFILE ${MAKE_ARGS[*]:-}"
make -f "$MAKEFILE" "${MAKE_ARGS[@]}"

if [ "$DO_PKG" -eq 1 ]; then
	echo "==> $(basename "$PKG_SCRIPT")"
	bash "$PKG_SCRIPT"
	echo
	echo "==> Built: etr.pkg ($(stat -c%s "$ROOT/etr.pkg") bytes), src/etr.self"
fi
