#!/usr/bin/env bash
#
# Fast PS3 dev iteration: rebuild the PPU binary, sign it as NPDRM, and
# upload EBOOT.BIN over FTP to the running PS3. No .pkg rebuild, no
# reinstall -- just hotswap the executable.
#
# Run this from the host. Requires the ps3dev toolchain (PSL1GHT) at
# $PS3DEV providing ppu-gcc, ppu-strip, sprxlinker, make_self_npdrm, curl
# on PATH. Usage:
#
#   src/ps3/tools/dev_deploy.sh
#
# Override the build configuration by setting env vars:
#
#   DEMO_MODE=1 src/ps3/tools/dev_deploy.sh        # 10s auto-race + quit
#   DEMO_MODE=1 DEMO_SHOTS=1 src/ps3/tools/dev_deploy.sh  # demo + screenshots
#
# Override the destination by setting env vars:
#
#   PS3_FTP_HOST=192.168.1.245 \
#   PS3_FTP_USER=anonymous \
#   PS3_FTP_PASS= \
#   PS3_INSTALL_DIR=/dev_hdd0/game/EXTR00001/USRDIR \
#   src/ps3/tools/dev_deploy.sh

set -euo pipefail

# --- config (override via env) ------------------------------------------------
PS3_FTP_HOST="${PS3_FTP_HOST:-192.168.1.245}"
PS3_FTP_USER="${PS3_FTP_USER:-anonymous}"
PS3_FTP_PASS="${PS3_FTP_PASS:-}"
PS3_INSTALL_DIR="${PS3_INSTALL_DIR:-/dev_hdd0/game/EXTR00001/USRDIR}"
CONTENT_ID="${CONTENT_ID:-UP0001-EXTR00001_00-0000000000000001}"
PS3DEV="${PS3DEV:-/usr/local/ps3dev}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LOCAL_ELF="${ROOT}/src/etr.elf"

# Put the ps3dev compilers + host signing tools on PATH.
export PATH="$PS3DEV/ppu/bin:$PS3DEV/bin:$PATH"

# --- sanity checks ------------------------------------------------------------
[ -f "$LOCAL_ELF" ] || {
    echo "Missing $LOCAL_ELF -- run a full PS3 build first." >&2
    exit 1
}
for tool in ppu-strip sprxlinker make_self_npdrm curl; do
    command -v "$tool" >/dev/null || {
        echo "$tool not found on PATH (expected under \$PS3DEV)." >&2
        echo "PS3DEV=$PS3DEV -- check your local ps3dev install." >&2
        exit 1
    }
done

# --- [1/4] Rebuild PPU binary -------------------------------------------------
echo "[1/4] Rebuilding PPU binary"
cd "$ROOT/src"
BUILD_LOG="$(mktemp)"
trap 'rm -f "$BUILD_LOG"' EXIT
# Forward DEMO_MODE / DEMO_SHOTS from the environment. Defaults match the
# Makefile (both off) so unset = menu-driven production build.
DEMO_MODE_ARG="${DEMO_MODE-}"
DEMO_SHOTS_ARG="${DEMO_SHOTS-}"
if ! make -f Makefile.ps3 \
        ${DEMO_MODE_ARG:+DEMO_MODE=$DEMO_MODE_ARG} \
        ${DEMO_SHOTS_ARG:+DEMO_SHOTS=$DEMO_SHOTS_ARG} \
        -j"$(nproc)" >"$BUILD_LOG" 2>&1; then
    echo "=== BUILD FAILED -- full output: ==="
    cat "$BUILD_LOG"
    exit 1
fi

# --- [2/4] Sign EBOOT.BIN -----------------------------------------------------
echo "[2/4] Signing EBOOT.BIN (ppu-strip + sprxlinker + make_self_npdrm)"
rm -f /tmp/etr.elf /tmp/EBOOT.BIN
ppu-strip -o /tmp/etr.elf "$LOCAL_ELF"
sprxlinker /tmp/etr.elf
make_self_npdrm /tmp/etr.elf /tmp/EBOOT.BIN "$CONTENT_ID"
ls -la /tmp/EBOOT.BIN

# --- [3/4] Upload over FTP ----------------------------------------------------
echo "[3/4] Uploading to ftp://${PS3_FTP_HOST}${PS3_INSTALL_DIR}/EBOOT.BIN"
curl --connect-timeout 10 --max-time 120 --ftp-pasv \
    -T /tmp/EBOOT.BIN \
    --user "${PS3_FTP_USER}:${PS3_FTP_PASS}" \
    "ftp://${PS3_FTP_HOST}${PS3_INSTALL_DIR}/EBOOT.BIN"
echo "Upload OK."

# --- [4/4] Verify remote file size --------------------------------------------
echo "[4/4] Verifying remote file size"
LOCAL_SIZE=$(stat -c %s /tmp/EBOOT.BIN)
REMOTE_SIZE=$(curl -sI --max-time 10 --ftp-pasv \
    --user "${PS3_FTP_USER}:${PS3_FTP_PASS}" \
    "ftp://${PS3_FTP_HOST}${PS3_INSTALL_DIR}/EBOOT.BIN" \
    | awk 'tolower($1) == "content-length:" {print $2}' \
    | tr -d '\r\n')
if [ -z "$REMOTE_SIZE" ]; then
    echo "ERROR: Could not read remote EBOOT.BIN size (FTP HEAD failed)." >&2
    echo "       Local size was $LOCAL_SIZE bytes. The PS3 may be off," >&2
    echo "       FTP may be down, or the upload silently failed." >&2
    exit 1
fi
if [ "$LOCAL_SIZE" != "$REMOTE_SIZE" ]; then
    echo "ERROR: EBOOT.BIN size mismatch after upload." >&2
    echo "       Local : $LOCAL_SIZE bytes" >&2
    echo "       Remote: $REMOTE_SIZE bytes" >&2
    echo "       The PS3 is still running the previous build." >&2
    exit 1
fi
echo "Verified: $LOCAL_SIZE bytes on both sides."
