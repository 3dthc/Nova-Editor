-- nova_bridge_restore: Lua device script that reinstalls the nova-bridge MIDI service after an MPC OS/Hakai
-- update replaces the system drive. MPC loads every device script in "MPC Documents/Devices/" at every start,
-- and this one lives on the persistent content drive, so it survives an update even though nova-bridge's own
-- systemd unit (in /usr/lib/systemd/system) does not.
--
-- Runs inside MPC's Lua VM (io=nil, os=nil; package.loadlib is the only way out, same as the "Addin Restore"
-- device script this is modeled on). All real work happens in nova_bridge_restore.so.
--
-- EXPERIMENTAL: this device-script mechanism is confirmed on Gen2 only (tools/mpc_patch/addin_restore in
-- mpc-vst-plugins); it should work the same way on Gen1, since both run the same MPC app and Lua engine, but
-- that has not yet been verified on real Gen1 hardware. Check /data/nova-bridge/restore.log after installing.

local function find_module()
    if debug and debug.getinfo then
        local src = debug.getinfo(1, "S").source
        if src and src:sub(1,1) == "@" then
            local dir = src:sub(2):match("(.*/)")
            if dir then return dir .. "nova_bridge_restore.so" end
        end
    end
    for _, root in ipairs({"/sdcard", "/nvme", "/data/UserData"}) do
        local path = root .. "/MPC Documents/Devices/Nova Bridge Restore/nova_bridge_restore.so"
        local f = package.loadlib(path, "*")
        if f then return path end
    end
    return nil
end

local function log(msg)
    if print then pcall(print, "[nova-bridge-restore] " .. tostring(msg)) end
end

local function try_restore()
    log("try_restore start")
    if not package or type(package.loadlib) ~= "function" then
        log("no package.loadlib")
        return "no-loadlib"
    end
    local module = find_module()
    log("module path: " .. tostring(module))
    if not module then
        log("could not find nova_bridge_restore.so")
        return "no-module"
    end
    local fn, err1, err2 = package.loadlib(module, "nova_bridge_restore_check")
    log("loadlib: fn=" .. tostring(fn) .. " err1=" .. tostring(err1) .. " err2=" .. tostring(err2))
    if type(fn) ~= "function" then
        log("loadlib failed")
        return "load-failed"
    end
    log("calling nova_bridge_restore_check")
    local ok, result = pcall(fn)
    log("result: ok=" .. tostring(ok) .. " val=" .. tostring(result))
    return "done:" .. tostring(result)
end

log("script loaded")
local r = try_restore()
log("direct: " .. tostring(r))

if nova_bridge_restore ~= nil then
    log("registering OnLoad/OnUnload")
    function nova_bridge_restore.OnLoad()
        log("OnLoad")
        return try_restore()
    end
    function nova_bridge_restore.OnUnload()
        log("OnUnload")
    end
end

return "helper-registered"
