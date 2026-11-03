# nova-bridge 0.1.0

Makes a Novation MiniNova on USB a shared MIDI port ("MiniNova MIDI") on an MPC (Gen 1, MPC OS 3.x, root access).
The MPC app plays and records it like any MIDI device, and the Patchy editor can exchange patches with it at the same time.

## Install
1. On the PC, unzip this, then copy the folder to the MPC (PowerShell):
   `scp -r nova-bridge root@<MPC IP>:/data/`
2. Over SSH on the MPC: `sh /data/nova-bridge/install.sh`
3. On the MPC: Preferences > MIDI, turn on Track for "MiniNova MIDI" (once). Use "MiniNova MIDI" as your tracks' output.

After an MPC OS or Hakai update, run step 2 again. Log: `/data/nova-bridge/bridge.log`.
Remove: `sh /data/nova-bridge/uninstall.sh`.
