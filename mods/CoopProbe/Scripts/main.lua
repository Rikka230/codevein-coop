-- CoopProbe: fine-grained log of who ends a co-op session (host and guest run it).
-- Every 0.2 s it logs the session/connection/death state when it changes, with milliseconds, and it logs
-- the engine's leave/kick/travel calls. It writes a flag file for CoopNative and, only with spectate_cam=1, moves the camera.
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
-- A boss kill must close the window at once: the outro calls EndCooperativeMode ~2 s after MulticastGainRewards, and a veto
-- there stalls the host on a black screen (session 7). Closed for 60 s after the rewards.
local rewardsAt = -1000
pcall(function()
    RegisterHook("/Script/Extensions.BossAreaBase:MulticastGainRewards", function()
        rewardsAt = os.clock()
        local f = io.open(FLAGS, "w"); if f then f:write("0"); f:close() end
        lastWindow = false
        -- A player held by the veto never retries leaving, so the host's outro would wait for him forever: if our own
        -- character is dead, have CoopNative replay the vetoed end now (he goes home as in vanilla and rejoins later).
        local pc = FindFirstOf("ActionPlayerController_C")
        local mine = valid(pc) and try(function() return pc.Pawn end) or nil
        local helper = StaticFindObject("/Script/Extensions.Default__CharacterHelper")
        local dead = not valid(mine) or try(function() return helper:IsDeath(mine) end)
        if dead then
            local c = io.open(WIN64 .. "\\plugins\\CoopNative.cmd", "w"); if c then c:write("replay"); c:close() end
        end
        log("boss rewards: spectate window forced closed" .. (dead and ", own character dead -> replay requested" or ""))
    end)
end)
local function spectateWindow()
    if os.clock() - rewardsAt < 60 then return false end
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

-- Spectate camera (session 7 experiment, only with spectate_cam=1 in plugins\CoopNative.ini). With the veto holding, the dead
-- player's own death sequence stops at its fade/loading screen (BP_FadeUIComponent) while still connected. In the spectate window,
-- if our own character is dead: lift the fade and point our camera at a living player. The view target is local to each PC.
local INI = WIN64 .. "\\plugins\\CoopNative.ini"
local function spectateCamEnabled()
    local f = io.open(INI, "r"); if not f then return false end
    local s = f:read("*a") or ""; f:close()
    return s:find("spectate_cam=1", 1, true) ~= nil
end
local camTarget, census, spectateStart = nil, 0, nil
-- Each step runs on its own and logs its first error (session 7: one silent error stopped the camera switch).
local errs = {}
local function step(what, f)
    local ok, e = pcall(f)
    if not ok and not errs[what] then errs[what] = true; log("spectate: " .. what .. " ERROR " .. tostring(e):sub(1, 160)) end
end
-- The unfade calls fail from Lua, so hide the death overlays themselves while spectating and restore them afterwards.
local OVERLAYS = { "WDT_FadeScreen_C", "WDT_EffectFadeScreen_C", "WDT_DeathParts_C" }
local hidden = {}   -- widget name -> {widget, original visibility}
local function hideOverlays()
    for _, cls in ipairs(OVERLAYS) do
        for _, w in ipairs(FindAllOf(cls) or {}) do
            if valid(w) then
                local n = name(w)
                if not hidden[n] then
                    hidden[n] = { w, try(function() return w:GetVisibility() end) or 3 }
                    log("spectate: hiding " .. n .. " (visibility was " .. tostring(hidden[n][2]) .. ")")
                end
                w:SetVisibility(1)   -- ESlateVisibility::Collapsed
            end
        end
    end
end
local camCleared = false
local function clearCameraFade(pc)
    local cm = pc.PlayerCameraManager
    if not valid(cm) then return end
    local a = cm.FadeAmount
    if type(a) == "number" and a > 0 then
        cm:StopCameraFade()
        if not camCleared then camCleared = true; log("spectate: camera fade " .. tostring(a) .. " cleared") end
    end
end
local function restoreOverlays()
    for n, e in pairs(hidden) do
        if valid(e[1]) then pcall(function() e[1]:SetVisibility(e[2]) end) end
        log("spectate: restored " .. n)
    end
    hidden = {}
end
-- Session 7: spectating works, but the dead player keeps his own low-health effects: a bloody screen filter (screen effects)
-- and the crisis heartbeat (ExcitingBgmManager). Remove the screen effects and switch the crisis mode off while spectating.
local bgmSaved, fxLogged = nil, false
local function quietDeathEffects()
    local n = 0
    for _, c in ipairs(FindAllOf("ScreenEffectManagerComponent") or {}) do
        if valid(c) and try(function() return c:IsActiveInstanceExists() end) ~= false then
            pcall(function() c:DestroyAllScreenEffects() end); n = n + 1
        end
    end
    if not fxLogged then fxLogged = true; log("spectate: cleared screen effects on " .. n .. " component(s)") end
    local bgm = FindFirstOf("ExcitingBgmManager")
    if valid(bgm) and not bgmSaved then
        bgmSaved = { bgm, bgm.bEnable, bgm.CrisisHealthRatio }
        bgm.CrisisHealthRatio = -1.0
        bgm.bEnable = false
        log("spectate: crisis BGM off (was enabled=" .. tostring(bgmSaved[2]) .. ", ratio=" .. tostring(bgmSaved[3]) .. ")")
    end
end
local function restoreDeathEffects()
    if bgmSaved and valid(bgmSaved[1]) then
        pcall(function() bgmSaved[1].CrisisHealthRatio = bgmSaved[3]; bgmSaved[1].bEnable = bgmSaved[2] end)
        log("spectate: crisis BGM restored")
    end
    bgmSaved, fxLogged = nil, false
end
-- Twice per spectate (at once, and 8 s later when the death fade has come): what is on screen (all user widgets with visibility), camera fade and view target.
local function screenCensus(pc)
    local parts = {}
    for _, w in ipairs(FindAllOf("UserWidget") or {}) do
        if valid(w) then
            local inVp = try(function() return w:IsInViewport() end)
            local vis = try(function() return w:GetVisibility() end)
            if inVp then parts[#parts + 1] = name(w) .. "=" .. tostring(vis) end
        end
    end
    local cm = try(function() return pc.PlayerCameraManager end)
    log("spectate census: fade=" .. tostring(try(function() return cm.FadeAmount end)) ..
        " viewtarget=" .. name(try(function() return pc:GetViewTarget() end)) ..
        " widgets in viewport: " .. table.concat(parts, " "))
end
local function spectateCam(window)
    if not window then
        if next(hidden) then restoreOverlays() end
        if bgmSaved then restoreDeathEffects() end
        camTarget, census, spectateStart, camCleared, errs = nil, 0, nil, false, {}
        return
    end
    if not spectateCamEnabled() then return end
    local pc = FindFirstOf("ActionPlayerController_C")
    if not valid(pc) then return end
    local helper = StaticFindObject("/Script/Extensions.Default__CharacterHelper")
    local mine = try(function() return pc.Pawn end)
    local mineDead = not valid(mine) or try(function() return helper:IsDeath(mine) end)
    if not mineDead then return end
    local target
    for _, p in ipairs(FindAllOf("BP_Player_C") or {}) do
        if valid(p) and not (valid(mine) and name(p) == name(mine)) and not try(function() return helper:IsDeath(p) end) then target = p; break end
    end
    if not target then return end
    step("overlays", hideOverlays)
    step("camera fade", function() clearCameraFade(pc) end)
    step("death effects", quietDeathEffects)
    step("view target", function()
        local vt = try(function() return pc:GetViewTarget() end)
        if camTarget ~= name(target) or not valid(vt) or name(vt) ~= name(target) then
            pc:SetViewTargetWithBlend(target, 0.5, 0, 0, false)
            if camTarget ~= name(target) then log("spectate: camera -> " .. name(target)) end
            camTarget = name(target)
        end
    end)
    spectateStart = spectateStart or os.clock()
    if census == 0 or (census == 1 and os.clock() - spectateStart > 8) then
        census = census + 1; step("census", function() screenCensus(pc) end)
    end
end

-- Group rest (experiment, host only, group_rest=1 in plugins\CoopNative.ini): when the host rests at a Mistle (the
-- EnemyLevelResetByCheckPoint channel closes for ~1 s, seen in session 7), every living player in the host's world is healed to
-- full. Health is server-side, so the host's SetHealth reaches the guest's character. Ichor/Regen live in the guest's save: not yet.
local function groupRestEnabled()
    local f = io.open(INI, "r"); if not f then return false end
    local s = f:read("*a") or ""; f:close()
    return s:find("group_rest=1", 1, true) ~= nil
end
local ENEMYRESET = 8
local lastReset, groupRestErr
local function groupRest()
    local sm = FindFirstOf("AppSessionManager")
    local gs = FindFirstOf("BP_AppGameState_C")
    if not (valid(sm) and valid(gs)) then return end
    if try(function() return gs.Role end) ~= 3 then return end                 -- host (authority) only
    local open = try(function() return sm:ExpectsBeOpenRaw(ENEMYRESET) end)
    local closedNow = lastReset == true and open == false
    lastReset = open
    if not closedNow or try(function() return gs.BossAreaState end) == 2 or not groupRestEnabled() then return end
    local helper = StaticFindObject("/Script/Extensions.Default__CharacterHelper")
    local healed = {}
    for _, p in ipairs(FindAllOf("BP_Player_C") or {}) do
        if valid(p) and not try(function() return helper:IsDeath(p) end) then
            local max = try(function() return helper:GetMaxHealth(p) end)
            if type(max) == "number" and max > 0 then
                local ok = pcall(function() helper:SetHealth(p, max) end)
                healed[#healed + 1] = name(p) .. (ok and ("=" .. string.format("%.0f", max)) or " FAILED")
            end
        end
    end
    log("group rest: Mistle rest seen, healed " .. table.concat(healed, ", "))
end

local last = ""
LoopAsync(200, function()
    ExecuteInGameThread(function()
        local line = try(stateLine) or "state error"
        if line ~= last then log("STATE " .. line); last = line end
        local w = try(spectateWindow) or false
        writeWindow(w)
        pcall(spectateCam, w)
        local ok, e = pcall(groupRest)
        if not ok and not groupRestErr then groupRestErr = true; log("group rest ERROR " .. tostring(e):sub(1, 160)) end
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
