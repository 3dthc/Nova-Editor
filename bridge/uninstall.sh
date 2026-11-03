#!/bin/sh
# Removes nova-bridge from the system drive (its folder in /data stays, so install.sh can put it back).
# To go back to the kernel-driver setup afterwards:  sh /data/mininova-usb.sh
say() { echo "nova-bridge: $*"; }
systemctl stop nova-bridge.service 2>/dev/null
mount -o remount,rw / || { say "could not make the system drive writable"; exit 1; }
rm -f /usr/lib/systemd/system/nova-bridge.service /usr/lib/systemd/system/acvs.service.wants/nova-bridge.service
rm -rf /usr/lib/nova-bridge
sync
mount -o remount,ro / 2>/dev/null
systemctl daemon-reload
say "removed. Unplug and replug the MiniNova's USB (or run: sh /data/mininova-usb.sh for the kernel-driver setup)."
