#!/bin/sh
# Removes the "Nova Bridge Restore" device script from the content drive. nova-bridge itself
# (and its systemd unit, if currently installed) is unaffected.
set -eu
say() { echo "nova-bridge-restore: $*"; }
find_content_root() {
    for r in /sdcard /nvme /data/UserData; do
        [ -d "$r/MPC Documents" ] && echo "$r" && return 0
    done
    return 1
}
ROOT=$(find_content_root) || { say "cannot find a content root"; exit 1; }
DEST="$ROOT/MPC Documents/Devices/Nova Bridge Restore"
if [ ! -d "$DEST" ]; then say "not installed, nothing to do"; exit 0; fi
rm -f "$DEST/nova_bridge_restore.so" "$DEST/nova_bridge_restore.lua" "$DEST/nova_bridge_restore.ini"
rmdir "$DEST" 2>/dev/null || true
say "removed $DEST"
