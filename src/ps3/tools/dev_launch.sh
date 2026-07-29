#!/usr/bin/env bash
#
# Mount and auto-play the installed ETR PKG on the PS3 via webMAN MOD's
# HTTP API. No FTP needed; this just hits two endpoints on the webMAN
# server.
#
# Usage:
#   src/ps3/tools/dev_launch.sh
#
# Defaults match dev_deploy.sh / dev_perflog.sh. Override via env if needed:
#   PS3_HOST=192.168.1.245 \
#   PS3_TITLE_ID=EXTR00001 \
#   src/ps3/tools/dev_launch.sh

set -euo pipefail

PS3_HOST="${PS3_HOST:-192.168.1.245}"
PS3_TITLE_ID="${PS3_TITLE_ID:-EXTR00001}"
TIMEOUT="${TIMEOUT:-30}"   # seconds to wait for the game to reach foreground
POLL="${POLL:-1}"          # seconds between polls

GAME_PATH="/dev_hdd0/game/${PS3_TITLE_ID}"

# /xmb.ps3$exit appears in cpursx.ps3 only while a game is foreground.
EXIT_MARKER='xmb.ps3$exit'

cpursx() {
    curl -s --connect-timeout 10 --max-time 15 "http://${PS3_HOST}/cpursx.ps3"
}

is_running() {
    cpursx | grep -qF "$EXIT_MARKER"
}

echo "[etr] mounting ${GAME_PATH} via webMAN..."
curl -fsL --connect-timeout 10 --max-time 20 \
    "http://${PS3_HOST}/mount.ps3${GAME_PATH}" -o /dev/null

echo "[etr] launching via /play.ps3..."
curl -fsL --connect-timeout 10 --max-time 20 \
    "http://${PS3_HOST}/play.ps3" -o /dev/null

# play.ps3 returns before the game is actually foreground; poll cpursx
# until the game's "Exit" button renders (i.e. the game is running).
elapsed=0
while ! is_running; do
    if [ "$elapsed" -ge "$TIMEOUT" ]; then
        echo "[etr] timed out after ${TIMEOUT}s waiting for ${PS3_TITLE_ID} to start" >&2
        exit 1
    fi
    sleep "$POLL"
    elapsed=$((elapsed + POLL))
done

echo "[etr] ${PS3_TITLE_ID} running on ${PS3_HOST} (foreground after ${elapsed}s)"
