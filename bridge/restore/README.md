# Nova Bridge Restore

Keeps [nova-bridge](../README.md) working across an MPC OS / Hakai update, without re-running
`install.sh` by hand over SSH every time.

## The problem

`nova-bridge`'s own `install.sh` writes its systemd unit into `/usr/lib/systemd/system/` on the
**system drive**. An MPC OS or Hakai update replaces that drive wholesale, so the unit disappears
and the MiniNova stops being recognized until someone SSHes in and runs `install.sh` again. Only
`/data/nova-bridge` (the installer itself) survives an update.

## What this does

Installs a Lua device script on the persistent content drive (`/sdcard`, `/nvme`, or
`/data/UserData`, whichever has an `MPC Documents` folder). MPC loads every device script in
`MPC Documents/Devices/` at every start. The script:

1. Checks whether `nova-bridge.service` is active.
2. If not, and `/data/nova-bridge/install.sh` exists, runs it (no MPC restart needed --
   `install.sh` activates the bridge and hands the MiniNova over from the kernel driver live).
3. Logs every step to `/data/nova-bridge/restore.log`.

So the first MPC start after any future OS/Hakai update fixes itself. No SSH, no remembering to
re-run anything.

**This is modeled directly on `tools/mpc_patch/addin_restore` in `mpc-vst-plugins`** (the same
Lua-device-script trick that keeps `LD_PRELOAD` addins alive across reboots on Gen2), adapted for
a systemd *service* instead of an `LD_PRELOAD` drop-in, and for the "update wipes it" trigger
instead of "every reboot wipes it."

## Status: EXPERIMENTAL, unverified on real hardware

The Lua device-script mechanism itself is confirmed working on Gen2 only (that's what
`addin_restore` has been tested on). It should behave the same way on Gen1 -- both run the same
MPC app and the same Lua engine -- but that has **not actually been checked on a Gen1 device**.
Offline: the C logic (`nova_bridge_restore.c`) passes a host test under ASan/UBSan against a fake
`systemctl` and a fake `install.sh`, covering both the "already active" and "needs reinstall"
branches. What's unverified is whether MPC's Lua sandbox on Gen1 loads and runs the script at all.

**If you install this: reboot once, then check `/data/nova-bridge/restore.log`.** You should see
either `restore: nova-bridge.service already active, nothing to do` (if it was already fine) or
the reinstall lines. If the file never appears at all, the device script isn't loading on Gen1 the
way it does on Gen2, and this needs a different approach -- report that back rather than assuming
it's silently working.

## Install

Copy this folder to the device, then as root:

```sh
scp -r restore root@<MPC IP>:/data/nova-bridge/restore
ssh root@<MPC IP> sh /data/nova-bridge/restore/install.sh
```

Remove: `sh /data/nova-bridge/restore/uninstall.sh`.

## Building `nova_bridge_restore.so`

Cross-compiled for armv7 (Gen1) by `.github/workflows/restore-build.yml` (`workflow_dispatch`,
uploads the three files as a run artifact -- no release, since this isn't a catalog plugin). To
build by hand instead, with Docker and QEMU for arm/v7:

```sh
docker run --rm --platform linux/arm/v7 -v "$PWD":/w -w /w arm32v7/gcc:11-bullseye sh -c '
  gcc -shared -fPIC -fvisibility=hidden -std=gnu11 -O2 -Wall -Wextra \
      -o nova_bridge_restore.so nova_bridge_restore.c
  strip nova_bridge_restore.so'
```
