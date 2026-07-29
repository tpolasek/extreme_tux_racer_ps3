#!/usr/bin/env bash
#
# Fetch etr_perf.log from the PS3 over FTP and print it to stdout.
#
# Usage:
#   src/ps3/tools/dev_perflog.sh
#
# Defaults match dev_deploy.sh. Override via env vars if needed:
#   PS3_FTP_HOST=192.168.1.245 \
#   PS3_INSTALL_DIR=/dev_hdd0/game/EXTR00001/USRDIR \
#   src/ps3/tools/dev_perflog.sh

set -euo pipefail

PS3_FTP_HOST="${PS3_FTP_HOST:-192.168.1.245}"
PS3_FTP_USER="${PS3_FTP_USER:-anonymous}"
PS3_FTP_PASS="${PS3_FTP_PASS:-}"
PS3_INSTALL_DIR="${PS3_INSTALL_DIR:-/dev_hdd0/game/EXTR00001/USRDIR}"

REMOTE="ftp://${PS3_FTP_HOST}${PS3_INSTALL_DIR}/etr_perf.log"

curl -s --connect-timeout 10 --max-time 30 --ftp-pasv \
    --user "${PS3_FTP_USER}:${PS3_FTP_PASS}" \
    "$REMOTE"
