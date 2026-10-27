-- CoopProbe: read-only, fine-grained log of who ends a co-op session (host and guest run it).
-- Every 0.2 s it logs the session/connection/death state when it changes, with milliseconds, and it logs
-- the engine's leave/kick/travel calls. It changes nothing in the game; it only writes a flag file for CoopNative.
local TAG = "[CoopProbe] "
local t0wall, t0clock = os.time(), os.clock()
local function stamp()
    local e = os.clock() - t0clock
    local s = t0wall + math.floor(e)
    return os.date("%H:%M:%S", s) .. string.format(".%03d", math.floor((e % 1) * 1000))
end
local function log(s) print(TAG .. stamp() .. " " .. tostring(s) .. "\n") end
local function try(f) local ok, v = pcall(f); if ok then return v end return nil end
local function valid(o) return o ~= nil and try(function() return o:IsValid() end) end
local function name(o) return try(function() return o:GetFName():ToString() end) or "?" end

local function stateLine()
    local sm = FindFirstOf("AppSessionManager")
    local gs = FindFirstOf("BP_AppGameState_C")
    if not (valid(sm) and valid(gs)) then return "no world" end
    local helper = StaticFindObject("/Script/Extensions.Default__CharacterHelper")
    local players = {}
    for _, p in ipairs(FindAllOf("BP_Player_C") or {}) do
        if valid(p) then
            local l = "ok"
            if try(function() return helper:IsDeath(p) end) or try(function() return helper:IsDying(p) end) then l = "dead"
            elseif try(function() return helper:IsDowned(p) end) then l = "downed" end
            players[#players + 1] = name(p) .. ":" .. l
        end
    end
    local conns = {}
    for _, c in ipairs(FindAllOf("NetConnection") or {}) do if valid(c) then conns[#conns + 1] = name(c) end end
    return string.format("role=%s multi=%s created=%s signal=%s boss=%s conns=[%s] players=[%s]",
        tostring(try(function() return gs.Role end)),
        tostring(try(function() return sm:IsOnlineMultiplay(gs) end)),
        tostring(try(function() return sm:IsCreateSession(gs) end)),
        tostring(try(function() return sm:GetRescueSignalState(gs) end)),
        tostring(try(function() return gs.BossAreaState end)),
        table.concat(conns, ","), table.concat(players, ","))
end

-- Spectate window for CoopNative's EndCooperativeMode veto: boss fight running, at least one player dead and one alive.
-- Same rule on host and guest (on the guest PC the dead one is the guest itself). Written to <Win64>\plugins\CoopNative.flags
-- ("1"/"0"); CoopNative only acts on it when its own ini enables the veto.
local WIN64 = debug.getinfo(1, "S").source:match("^@(.*)[/\\]ue4ss[/\\]Mods[/\\]") or "."
local FLAGS = WIN64 .. "\\plugins\\CoopNative.flags"
local lastWindow
local function spectateWindow()
    local gs = FindFirstOf("BP_AppGameState_C")
    if not valid(gs) or try(function() return gs.BossAreaState end) ~= 2 then return false end
    local helper = StaticFindObject("/Script/Extensions.Default__CharacterHelper")
    local dead, alive = false, false
    for _, p in ipairs(FindAllOf("BP_Player_C") or {}) do
        if valid(p) then
            if try(function() return helper:IsDeath(p) end) then dead = true else alive = true end
        end
    end
    return dead and alive
end
local function writeWindow(w)
    if w == lastWindow then return end
    lastWindow = w
    local f = io.open(FLAGS, "w")
    if f then f:write(w and "1" or "0"); f:close() end
    log("spectate window " .. (w and "OPEN" or "closed") .. (f and "" or " (could not write " .. FLAGS .. ")"))
end

local last = ""
LoopAsync(200, function()
    ExecuteInGameThread(function()
        local line = try(stateLine) or "state error"
        if line ~= last then log("STATE " .. line); last = line end
        writeWindow(try(spectateWindow) or false)
    end)
    return false
end)

-- engine leave/kick/travel paths (client RPCs run on the guest; game mode calls on the host)
local HOOKS = {
    "/Script/Engine.PlayerController:ClientWasKicked",
    "/Script/Engine.PlayerController:ClientReturnToMainMenu",
    "/Script/Engine.PlayerController:ClientTravel",
    "/Script/Engine.PlayerController:ClientTravelInternal",
    "/Script/Engine.PlayerController:ServerNotifyLoadedWorld",
    "/Script/Engine.GameModeBase:ReturnToMainMenuHost",
    "/Script/Engine.GameModeBase:K2_OnLogout",
}
for _, fn in ipairs(HOOKS) do
    local short = fn:match("[^.]+:[^:]+$") or fn
    local ok, err = pcall(function()
        RegisterHook(fn, function(ctx, ...)
            local args = {}
            for _, p in ipairs({...}) do
                local v = try(function() return p:get() end)
                if type(v) == "userdata" then v = try(function() return v:ToString() end) or try(function() return v:GetFName():ToString() end) or "obj" end
                args[#args + 1] = tostring(v)
            end
            log("CALL " .. short .. "(" .. table.concat(args, ", ") .. ") on " .. name(try(function() return ctx:get() end)))
        end)
    end)
    log("hook " .. short .. (ok and " ok" or (" FAILED " .. tostring(err):sub(1, 90))))
end
log("loaded")
