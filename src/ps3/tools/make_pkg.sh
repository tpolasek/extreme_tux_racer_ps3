#!/usr/bin/env bash
#
# Packages the ETR PS3 (PPU) executable + game data into an installable .pkg.
#
# Run from the host with the ps3dev tools (ppu-strip, sprxlinker,
# make_self_npdrm, sfo.py, pkg.py) on PATH — i.e. with $PS3DEV set and
# $PS3DEV/ppu/bin:$PS3DEV/bin prepended to PATH. Paths are resolved relative
# to the repository root so the script can be invoked from anywhere.
#
# Usage:
#   src/ps3/tools/make_pkg.sh [elf] [data_dir] [out_pkg]
#
#   <elf>       default: src/etr.elf        (built by `make -f src/Makefile.ps3`)
#   <data_dir>  default: data               (game assets; bundled as USRDIR/data)
#   <out_pkg>   default: etr.pkg
#
set -euo pipefail

# Resolve repo root (this script lives in <root>/src/ps3/tools/).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

ELF="${1:-$ROOT/src/etr.elf}"
DATA_DIR="${2:-$ROOT/data}"
OUT_PKG="${3:-$ROOT/etr.pkg}"

# Title metadata. TITLE_ID ETR00001 MUST match the data_dir hardcoded in
# game_config.cpp::InitConfig() (the OS installs the pkg to
# /dev_hdd0/game/<TITLE_ID>/USRDIR).
TITLE="Extreme Tux Racer"
TITLE_ID="EXTR00001"
APP_VER="01.00"
CONTENT_ID="UP0001-${TITLE_ID}_00-0000000000000001"

# XMB icon. Prefer a project logo if present, else the ps3dev default.
ICON_PNG=""
for cand in "$ROOT/resources/etr.png" "$ROOT/src/ps3/tools/ICON0.PNG"; do
	if [ -f "$cand" ]; then ICON_PNG="$cand"; break; fi
done
if [ -z "$ICON_PNG" ]; then
	ICON_PNG="${PS3DEV:-/usr/local/ps3dev}/bin/ICON0.PNG"
fi

WORK="$(mktemp -d)"
PKG_DIR="${WORK}/pkg"
USRDIR="${PKG_DIR}/USRDIR"
mkdir -p "${USRDIR}"

TOTAL=7
N=0
step() { N=$((N+1)); echo "[${N}/${TOTAL}] $1"; }

step "Verifying inputs"
[ -f "${ELF}" ]      || { echo "missing ELF: ${ELF}" >&2; exit 1; }
[ -d "${DATA_DIR}" ] || { echo "missing data dir: ${DATA_DIR}" >&2; exit 1; }
[ -f "${ICON_PNG}" ] || { echo "missing icon: ${ICON_PNG}" >&2; exit 1; }

step "ppu-strip + sprxlinker"
cp "${ELF}" "${WORK}/etr.raw"
ppu-strip -o "${WORK}/etr.elf" "${WORK}/etr.raw"
sprxlinker "${WORK}/etr.elf"

step "make_self_npdrm -> pkg/USRDIR/EBOOT.BIN"
make_self_npdrm "${WORK}/etr.elf" "${USRDIR}/EBOOT.BIN" "${CONTENT_ID}"

step "Copying data/ -> pkg/USRDIR/data"
cp -a "${DATA_DIR}" "${USRDIR}/data"

step "Copying icon -> pkg/ICON0.PNG"
cp -a "${ICON_PNG}" "${PKG_DIR}/ICON0.PNG"

step "Generating PARAM.SFO"
SFO_XML="${WORK}/sfo.xml"
cp "${PS3DEV:-/usr/local/ps3dev}/bin/sfo.xml" "${SFO_XML}"
sed -i "s/01\.00/${APP_VER}/g" "${SFO_XML}"
sfo.py --title "${TITLE}" --appid "${TITLE_ID}" -f "${SFO_XML}" "${PKG_DIR}/PARAM.SFO"

step "pkg.py -> ${OUT_PKG}"
pkg.py --contentid "${CONTENT_ID}" "${PKG_DIR}/" "${OUT_PKG}"

echo
echo "Done."
echo "  Title:       ${TITLE}"
echo "  Title ID:    ${TITLE_ID}"
echo "  App version: ${APP_VER}"
echo "  Content ID:  ${CONTENT_ID}"
echo "  Package:     ${OUT_PKG} ($(stat -c%s "${OUT_PKG}") bytes)"

rm -rf "${WORK}"
