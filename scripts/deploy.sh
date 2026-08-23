#!/usr/bin/env bash
#
# Install the 4K EQ module on the Move SAFELY.
#
# Critical: never scp directly over a live .so. The shim dlopen()s it, so
# overwriting the file mutates the mmap'd code pages of a running process —
# which segfaults the whole firmware. Upload to a temp name, then mv:
# rename(2) is atomic and leaves the old inode intact for the running process.
#
# And a NEW FILE ON DISK IS NOT THE RUNNING ONE. The atomic mv swaps the
# directory entry while the chain host keeps the old inode mapped, so this
# script also asks the slot to re-load. Without that step an on-device
# loadtest passes against code the device is not playing.
#
#   ./scripts/deploy.sh [host] [track-slot] [fx-position]
set -euo pipefail

HOST="${1:-move.local}"
SLOT="${2:-0}"
FXPOS="${3:-1}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
DEST="/data/UserData/schwung/modules/audio_fx/4k-eq"

SO="4k-eq.so"   # audio_fx: the chain host dlopens <id>/<id>.so, never dsp.so
[ -f "$SRC/build/$SO" ] || { echo "no build/$SO — run ./scripts/build.sh first" >&2; exit 1; }

echo "==> $HOST:$DEST"
ssh "$HOST" "mkdir -p $DEST"

scp -q "$SRC/build/$SO"           "$HOST:$DEST/$SO.new"
scp -q "$SRC/src/module.json"     "$HOST:$DEST/module.json.new"
scp -q "$SRC/src/help.json"       "$HOST:$DEST/help.json.new"
scp -q "$SRC/src/web_ui.html"     "$HOST:$DEST/web_ui.html.new"
scp -q "$SRC/src/movy_config.json" "$HOST:$DEST/movy_config.json.new"

# Atomic swap. Do NOT replace this with a direct scp.
ssh "$HOST" "cd $DEST && \
    mv -f $SO.new $SO && \
    mv -f module.json.new module.json && \
    mv -f help.json.new help.json && \
    mv -f web_ui.html.new web_ui.html && \
    mv -f movy_config.json.new movy_config.json && \
    chmod 755 $SO && rm -f dsp.so && ls -l $SO module.json"

# On-device loader test. Note what this does and does not prove: it dlopens
# the file ITSELF, so it validates the build — not what the slot is running.
# The reload below is what makes those the same thing.
if [ -f "$SRC/build/fkq_loadtest" ]; then
    scp -q "$SRC/build/fkq_loadtest" "$HOST:$DEST/fkq_loadtest.new"
    ssh "$HOST" "cd $DEST && mv -f fkq_loadtest.new fkq_loadtest && chmod 755 fkq_loadtest"
    echo "==> on-device loadtest"
    ssh "$HOST" "cd $DEST && ./fkq_loadtest ./$SO ./module.json" || {
        echo "!! loadtest FAILED on device — not reloading the slot" >&2; exit 1; }
fi

# deploy.sh may be given an ssh alias (move-usb); the reload speaks HTTP to the
# address, so resolve the alias to its HostName.
RHOST="$(ssh -G "$HOST" 2>/dev/null | awk '/^hostname /{print $2; exit}')"
RHOST="${RHOST:-$HOST}"
# The build fingerprint compiled into the .so we just shipped. The reload
# checks the RUNNING instance reports this same string — the only signal that
# does not lie when the chain host keeps an old inode mapped.
BUILD_ID="$(strings "$SRC/build/$SO" | grep -m1 '^FKQBUILD:')"
echo "==> reload fx$FXPOS on slot $SLOT ($RHOST) — expecting ${BUILD_ID:-?}"
python3 "$SRC/scripts/reload_fx_slot.py" "$RHOST" "$SLOT" "$FXPOS" 4k-eq "$BUILD_ID" || \
    echo "!! reload failed — re-pick the effect in the FX slot by hand, or the" \
         "device keeps running the OLD code" >&2
