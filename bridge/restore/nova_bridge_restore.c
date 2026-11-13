/* nova_bridge_restore_check: called from the "Nova Bridge Restore" Lua device script, which MPC loads from the
 * persistent content drive at every start (io=nil, os=nil in that sandbox; package.loadlib is the only way out).
 * That boot point survives an MPC OS / Hakai update even though /usr/lib/systemd/system (where nova-bridge's own
 * unit lives) does not -- an update replaces the whole system drive, which is exactly the problem this works
 * around: without it, nova-bridge stops working after every OS/Hakai update until someone re-runs install.sh by
 * hand over SSH.
 *
 * Lua calls this as a plain lua_CFunction (the loadlib(path, "name") convention: a C function taking lua_State*
 * and returning how many results it pushed). It pushes nothing, so the signature only needs to match in shape --
 * no Lua header is required. Logic: if nova-bridge.service is already active, do nothing; otherwise, if
 * /data/nova-bridge/install.sh is there, run it (it does its own remount/write/activate and does not need MPC
 * restarted). Every step is logged to /data/nova-bridge/restore.log for anyone checking after the fact.
 *
 * UNVERIFIED ON REAL HARDWARE as of writing: the Lua device-script mechanism this relies on is confirmed only on
 * Gen2 (tools/mpc_patch/addin_restore); it is expected to work the same way on Gen1 since both run the same MPC
 * app and Lua engine, but that has not been checked on an actual device yet. Test on a Gen1 unit before trusting it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define LOG_PATH "/data/nova-bridge/restore.log"
#define INSTALL_SH "/data/nova-bridge/install.sh"

static void log_line(const char *msg) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    fprintf(f, "%s\n", msg);
    fclose(f);
}

static int service_active(void) {
    /* systemctl is-active --quiet exits 0 only when the unit is loaded and running */
    int rc = system("systemctl is-active --quiet nova-bridge.service 2>/dev/null");
    return rc == 0;
}

__attribute__((visibility("default")))
int nova_bridge_restore_check(void *L) {
    (void)L;   /* unused: we push no results back to Lua, so no lua_State calls are needed */

    if (service_active()) {
        log_line("restore: nova-bridge.service already active, nothing to do");
        return 0;
    }

    if (access(INSTALL_SH, F_OK) != 0) {
        log_line("restore: " INSTALL_SH " not found, nothing to restore");
        return 0;
    }

    log_line("restore: nova-bridge.service not active, running install.sh");
    int rc = system("sh " INSTALL_SH " >>/data/nova-bridge/restore.log 2>&1");
    log_line(rc == 0 ? "restore: install.sh succeeded" : "restore: install.sh FAILED (see lines above)");
    return 0;
}
