#!/bin/sh
# Installs the "Nova Bridge Restore" device script on the content drive, so MPC re-runs
# /data/nova-bridge/install.sh by itself at every start if the nova-bridge service isn't active --
# the case right after an MPC OS / Hakai update, which replaces the system drive nova-bridge's own
# systemd unit lives on. Run on the MPC as root, with the three files (nova_bridge_restore.so,
# nova_bridge_restore.lua, nova_bridge_restore.ini) next to this script.
#
# EXPERIMENTAL: unverified on real Gen1 hardware. After installing, reboot once and check
# /data/nova-bridge/restore.log for lines starting "restore:". Report back what you see.
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
say() { echo "nova-bridge-restore: $*"; }

for f in nova_bridge_restore.so nova_bridge_restore.lua nova_bridge_restore.ini; do
    [ -f "$DIR/$f" ] || { say "$f is missing next to install.sh: nothing changed"; exit 1; }
done

find_content_root() {
    for r in /sdcard /nvme /data/UserData; do
        [ -d "$r/MPC Documents" ] && echo "$r" && return 0
    done
    return 1
}
ROOT=$(find_content_root) || { say "cannot find a content root (no 'MPC Documents' folder in /sdcard, /nvme or /data/UserData)"; exit 1; }
DEST="$ROOT/MPC Documents/Devices/Nova Bridge Restore"

mkdir -p "$DEST"
cp "$DIR/nova_bridge_restore.so" "$DEST/nova_bridge_restore.so.new" && mv "$DEST/nova_bridge_restore.so.new" "$DEST/nova_bridge_restore.so"
cp "$DIR/nova_bridge_restore.lua" "$DEST/nova_bridge_restore.lua.new" && mv "$DEST/nova_bridge_restore.lua.new" "$DEST/nova_bridge_restore.lua"
cp "$DIR/nova_bridge_restore.ini" "$DEST/nova_bridge_restore.ini.new" && mv "$DEST/nova_bridge_restore.ini.new" "$DEST/nova_bridge_restore.ini"
chmod 755 "$DEST/nova_bridge_restore.so"
chmod 644 "$DEST/nova_bridge_restore.lua" "$DEST/nova_bridge_restore.ini"

say "installed at: $DEST"
say "MPC picks it up at the next start. After that, if nova-bridge.service isn't active,"
say "MPC itself re-runs /data/nova-bridge/install.sh -- no SSH needed."
say "Check /data/nova-bridge/restore.log afterwards to confirm."
