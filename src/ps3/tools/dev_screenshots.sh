#!/usr/bin/env bash
#
# Fetch the DEMO_MODE screenshots from the PS3 over FTP into /tmp and
# print the local paths of the files that were retrieved.
#
# Usage:
#   src/ps3/tools/dev_screenshots.sh
#
# Defaults match dev_perflog.sh. Override via env vars if needed:
#   PS3_FTP_HOST=192.168.1.245 \
#   PS3_FTP_USER=anonymous \
#   PS3_INSTALL_DIR=/dev_hdd0/game/EXTR00001/USRDIR \
#   OUT_DIR=/tmp \
#   src/ps3/tools/dev_screenshots.sh

set -uo pipefail

PS3_FTP_HOST="${PS3_FTP_HOST:-192.168.1.245}"
PS3_FTP_USER="${PS3_FTP_USER:-anonymous}"
PS3_FTP_PASS="${PS3_FTP_PASS:-}"
PS3_INSTALL_DIR="${PS3_INSTALL_DIR:-/dev_hdd0/game/EXTR00001/USRDIR}"
OUT_DIR="${OUT_DIR:-/tmp}"

FILES=(demo_frame150.png demo_close.png)

rc=0
for f in "${FILES[@]}"; do
    out="${OUT_DIR}/${f}"
    if curl -fsL --connect-timeout 10 --max-time 30 --ftp-pasv \
        --user "${PS3_FTP_USER}:${PS3_FTP_PASS}" \
        "ftp://${PS3_FTP_HOST}${PS3_INSTALL_DIR}/${f}" -o "$out"; then
        echo "$out"
    else
        echo "missing: ${PS3_INSTALL_DIR}/${f}" >&2
        rc=1
    fi
done
exit $rc
