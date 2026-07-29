#!/usr/bin/env bash
#
# If ETR (or any game) is running on the PS3, close it back to XMB via
# webMAN MOD's HTTP API, then block until the XMB is foreground again.
# Idempotent: if nothing is running, exits 0 immediately.
#
# Use before dev_deploy.sh to release the lock on EBOOT.BIN so the FTP
# upload can replace it.
#
# Usage:
#   src/ps3/tools/dev_close.sh
#
# Detection: webMAN's /cpursx.ps3 shows an "Exit" button linking to
# /xmb.ps3$exit ONLY while a game is foreground. Its disappearance is the
# "back on XMB" signal.
#
# Defaults match dev_deploy.sh. Override via env if needed:
#   PS3_HOST=192.168.1.245 src/ps3/tools/dev_close.sh

set -euo pipefail

PS3_HOST="${PS3_HOST:-192.168.1.245}"
TIMEOUT="${TIMEOUT:-30}"   # seconds to wait for the game to exit
POLL="${POLL:-1}"          # seconds between polls

# /xmb.ps3$exit appears in cpursx.ps3 only while a game is running.
EXIT_MARKER='xmb.ps3$exit'

cpursx() {
    curl -s --connect-timeout 10 --max-time 15 "http://${PS3_HOST}/cpursx.ps3"
}

is_running() {
    cpursx | grep -qF "$EXIT_MARKER"
}

if ! is_running; then
    echo "[etr] no game running on ${PS3_HOST}"
    exit 0
fi

echo "[etr] game running on ${PS3_HOST}; sending /xmb.ps3\$exit..."
curl -fsL --connect-timeout 10 --max-time 20 \
    "http://${PS3_HOST}/xmb.ps3"\$exit -o /dev/null

elapsed=0
while is_running; do
    if [ "$elapsed" -ge "$TIMEOUT" ]; then
        echo "[etr] timed out after ${TIMEOUT}s waiting for exit" >&2
        exit 1
    fi
    sleep "$POLL"
    elapsed=$((elapsed + POLL))
done

echo "[etr] back on XMB (closed after ${elapsed}s)"
