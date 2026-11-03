#!/bin/sh
# nova-bridge installer. Run on the MPC as root:   sh /data/nova-bridge/install.sh
# Safe to run again at any time, e.g. after an MPC OS or Hakai update (those replace the system drive).
# What it does:
#   1. removes the old kernel-driver service (mininova-usb.service), which the bridge replaces
#   2. copies the bridge to the system drive and starts it at every boot, before the MPC app
#   3. hands the MiniNova from the kernel driver to the bridge right now (no reboot needed)
DIR=$(cd "$(dirname "$0")" && pwd)
BIN=/usr/lib/nova-bridge/nova-bridge
UNIT=/usr/lib/systemd/system/nova-bridge.service
WANTS=/usr/lib/systemd/system/acvs.service.wants
DRV=/sys/bus/usb/drivers/snd-usb-audio
say() { echo "nova-bridge: $*"; }

[ -f "$DIR/nova-bridge" ] || { say "the nova-bridge program is missing next to install.sh: nothing changed"; exit 1; }
systemctl cat acvs.service >/dev/null 2>&1 || { say "acvs.service not found: nothing changed"; exit 1; }
mount -o remount,rw / || { say "could not make the system drive writable: nothing changed"; exit 1; }

rm -f /usr/lib/systemd/system/mininova-usb.service "$WANTS/mininova-usb.service"
mkdir -p /usr/lib/nova-bridge "$WANTS"
cp "$DIR/nova-bridge" "$BIN.new" && chmod 755 "$BIN.new" && mv "$BIN.new" "$BIN"
cat > "$UNIT" <<UNITFILE
[Unit]
Description=MiniNova USB MIDI bridge (shared MIDI port "MiniNova MIDI")
Before=acvs.service

[Service]
ExecStart=$BIN --log $DIR/bridge.log
Restart=always
RestartSec=2

[Install]
WantedBy=acvs.service
UNITFILE
ln -sf ../nova-bridge.service "$WANTS/nova-bridge.service"
sync
mount -o remount,ro / 2>/dev/null
systemctl daemon-reload

# the kernel driver lets go of the MiniNova (the old new_id would otherwise keep it until a reboot)
echo "1235 001e" > "$DRV/remove_id" 2>/dev/null
for link in "$DRV"/*:1.0; do
    [ -e "$link" ] || continue
    ifc=$(basename "$link"); dev=${ifc%:1.0}
    if [ "$(cat /sys/bus/usb/devices/$dev/idVendor 2>/dev/null)" = 1235 ] && \
       [ "$(cat /sys/bus/usb/devices/$dev/idProduct 2>/dev/null)" = 001e ]; then
        echo "$ifc" > "$DRV/unbind" && say "released the MiniNova from the kernel driver"
    fi
done

systemctl restart nova-bridge.service
sleep 3
if systemctl is-active nova-bridge.service >/dev/null 2>&1; then
    say "running. Last log lines:"
    tail -n 3 "$DIR/bridge.log" 2>/dev/null
    say "done. In the MPC: Preferences > MIDI, turn on Track for \"MiniNova MIDI\" (once),"
    say "then set your MiniNova MIDI tracks' output to \"MiniNova MIDI\"."
else
    say "the service did not start. Send Claude the output of:  journalctl -u nova-bridge -n 30"
    exit 1
fi
