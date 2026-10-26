-- True Light - Real Torch Shadows.
-- The game lights a held torch with "PlayerAuxTorchLight", a light by Henry's head whose shadow covers only a cone
-- along the view and leaves out his body. That suits first person and breaks in any view of him from outside, so this
-- script mutes it and lights the scene from the flame instead.

TrueLight = TrueLight or {}
TrueLight.Version = "1.0.0"

local LOG_TAG = "[TrueLight] "
local CFG_PATH = "mods/true_light/mod.cfg"
local CMD_PREFIX = "true_light_"

local FAKE_LIGHT_NAME = "PlayerAuxTorchLight"
local FAKE_LIGHT_SLOT = 1
local THIRD_PERSON_MIN_CAMERA_DISTANCE = 0.6
-- The game can reload the fake light's slot, which brings its light back.
local REMUTE_INTERVAL = 2.0
-- The engine refreshes a light's shadow map every frame only while the camera is within this distance. The archetype's
-- 0 throttles it in third person, where a stale map shades the scene from where the light was.
local SHADOW_EVERY_FRAME_RADIUS = 1000

-- A respawned torch spends a frame at the world origin, then is raised from the hip or saddle. Lit during the raise, it
-- throws the reins' and the rider's shadows across the scene; left at the head once it is up, it throws the raised
-- arm's. So the flame takes the light once it is up, or still (a pose that holds it low), or after HANDOVER_MAX_DELAY.
local HELD_MAX_DISTANCE = 2.5
local RAISED_MIN_HEIGHT = -0.25
local SETTLED_MAX_SPEED = 0.5
local SETTLED_FRAMES = 3
local HANDOVER_MAX_DELAY = 0.6

-- A photomode_enable where photo mode cannot open never gets a close_photomode; script timers stop only while it is
-- open, so a tick this long after the press means it did not open.
local PHOTO_MODE_GRACE = 0.5

local LEFT_HAND, RIGHT_HAND = 1, 0
local LIGHT_HANDS = { LEFT_HAND, RIGHT_HAND }
local DEFAULT_FLAME_HELPER = "slt_particle"

local SETTINGS = {
    { key = "third_person", default = 1, help = "1 = real torch light in third person, 0 = the game's own" },
    { key = "first_person", default = 0, help = "1 = real torch light in first person, 0 = the game's own" },
    { key = "photo_mode", default = 1, help = "1 = real torch light in photo mode, 0 = the game's own" },
}

-- An unlit torch is held too, so a torch must burn. A lantern (class Lamp, from mods) gets the same fake light as a
-- torch, and its own glow casts no shadows.
local DEFAULT_HELD_LIGHTS = {
    { class = "Torch", state = "Active" },
    { class = "Lamp" },
}

local function Log(message)
    System.LogAlways(LOG_TAG .. message)
end

local function LengthSquared(deltaX, deltaY, deltaZ)
    return deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ
end

local function FindSetting(key)
    for _, setting in ipairs(SETTINGS) do
        if setting.key == key then return setting end
    end
end

function TrueLight:ApplyDefaults()
    self.cfg = {}
    for _, setting in ipairs(SETTINGS) do self.cfg[setting.key] = setting.default end
end

function TrueLight:CmdSetting(key, line)
    local value = tonumber(line)
    if value == nil then
        Log(string.format("%s%s = %s (%s)", CMD_PREFIX, key, tostring(self.cfg[key]), FindSetting(key).help))
        return
    end
    self.cfg[key] = (value ~= 0) and 1 or 0
    self.cfgCommandCount = (self.cfgCommandCount or 0) + 1
end

-- One function per setting: a command typed without a value expands %line to nothing, which leaves no room for a key
-- argument after it.
TrueLight.Set = {}
for _, setting in ipairs(SETTINGS) do
    local key = setting.key
    TrueLight.Set[key] = function(line) TrueLight:CmdSetting(key, line) end
end

local function DescribeHeldLights(heldLights)
    local descriptions = {}
    for index, heldLight in ipairs(heldLights) do
        local text = heldLight.class or ("name:" .. heldLight.namePrefix)
        if heldLight.helper then text = text .. " helper " .. heldLight.helper end
        if heldLight.state then text = text .. " state " .. heldLight.state end
        descriptions[index] = text
    end
    return table.concat(descriptions, "; ")
end

-- true_light_held_light <class | name:prefix> [helper | -] [state | -]
function TrueLight:CmdHeldLight(line)
    local match, helper, state = string.match(line or "", "^%s*(%S+)%s*(%S*)%s*(%S*)")
    if not match then
        Log(CMD_PREFIX .. "held_light: " .. DescribeHeldLights(self.heldLights))
        return
    end
    local heldLight = {}
    local namePrefix = string.match(match, "^name:(.+)$")
    if namePrefix then heldLight.namePrefix = namePrefix else heldLight.class = match end
    if helper ~= "" and helper ~= "-" then heldLight.helper = helper end
    if state ~= "" and state ~= "-" then heldLight.state = state end
    table.insert(self.heldLights, heldLight)
    self.cfgCommandCount = (self.cfgCommandCount or 0) + 1
end

function TrueLight:LoadCfg()
    self.cfgCommandCount = 0
    -- Copied so the cfg's additions never land in the defaults, and a reload starts again from them.
    self.heldLights = {}
    for index, heldLight in ipairs(DEFAULT_HELD_LIGHTS) do self.heldLights[index] = heldLight end
    pcall(System.ExecuteCommand, "exec " .. CFG_PATH)
    local values = {}
    for index, setting in ipairs(SETTINGS) do values[index] = setting.key .. " " .. tostring(self.cfg[setting.key]) end
    local source = self.cfgCommandCount > 0 and ("cfg read from " .. CFG_PATH) or "no cfg, defaults"
    Log(string.format("%s: %s; held lights: %s", source, table.concat(values, ", "), DescribeHeldLights(self.heldLights)))
end

local function MatchHeldLight(entity, heldLights)
    local name
    for _, heldLight in ipairs(heldLights) do
        local matches = heldLight.class ~= nil and entity.class == heldLight.class
        if not matches and heldLight.namePrefix then
            name = name or entity:GetName() or ""
            matches = string.sub(name, 1, #heldLight.namePrefix) == heldLight.namePrefix
        end
        if matches and (not heldLight.state or (entity.IsInState and entity:IsInState(heldLight.state))) then
            return heldLight
        end
    end
end

-- Looked up from the hands, not System.GetEntitiesByClass: that walks every entity (10 ms a call) and cost a third of
-- the frame rate when run every frame.
local function FindHeldLight(heldLights)
    local human = player and player.human
    if not human or not XGenAIModule then return nil end
    for _, hand in ipairs(LIGHT_HANDS) do
        local itemId = human:GetItemInHand(hand)
        local item = itemId and XGenAIModule.GetEntityByWUID(itemId)
        local heldLight = item and MatchHeldLight(item, heldLights)
        if heldLight then return item, heldLight end
    end
end

local function FlameHelper(item, heldLight)
    return heldLight.helper
        or (item.Properties and item.Properties.sHelper)
        or (item.Properties2 and item.Properties2.Helper)
        or DEFAULT_FLAME_HELPER
end

local function FlamePosition(item, heldLight)
    if item.nParticleSlot then return item:GetSlotWorldPos(item.nParticleSlot) end
    return item:GetSlotHelperPos(0, FlameHelper(item, heldLight), false)
end

-- TPVCamera's mod_camera_view covers its close shots near walls; the camera's distance from the head covers other
-- camera mods. pcall: the variable does not exist without TPVCamera.
local function IsThirdPerson()
    local found, view = pcall(System.GetCVar, "mod_camera_view")
    if found and type(view) == "number" and view >= 1 then return true end
    local head = player:GetBonePos("Head")
    local camera = System.GetViewCameraPos()
    if not head or not camera then return false end
    return LengthSquared(camera.x - head.x, camera.y - head.y, camera.z - head.z)
        > THIRD_PERSON_MIN_CAMERA_DISTANCE * THIRD_PERSON_MIN_CAMERA_DISTANCE
end

function TrueLight:OnAction(action, activation)
    if activation ~= "release" then return end
    if action == "photomode_enable" then
        self.photoMode, self.photoModeOpenedAt = true, System.GetCurrAsyncTime()
    elseif action == "close_photomode" then
        self.photoMode = false
    else
        return
    end
    -- Script timers stop while photo mode is open, so the light switches here.
    if self.started then pcall(self.Update, self, true) end
end

-- Wrapped once, as a rerun would wrap the wrapper; it calls through the global table so a rerun's OnAction is used.
local function HookPlayerActions()
    if TrueLight.actionsHooked or not Player then return end
    TrueLight.actionsHooked = true
    local originalOnAction = Player.OnAction
    function Player:OnAction(action, activation, value)
        if originalOnAction then originalOnAction(self, action, activation, value) end
        pcall(TrueLight.OnAction, TrueLight, action, activation, value)
    end
end

function TrueLight:WantsRealLight()
    if self.photoMode then return self.cfg.photo_mode == 1 end
    if IsThirdPerson() then return self.cfg.third_person == 1 end
    return self.cfg.first_person == 1
end

-- The fake light's Properties are its archetype's table, shared by every light of that archetype, so they are only
-- read into a fresh description.
local function FakeLightDesc(fakeLight, slot)
    return Light.GetLightDesc(fakeLight.Properties, slot)
end

local function AllDirectionsDesc(fakeLight)
    local desc = FakeLightDesc(fakeLight, -1)
    desc.spot_shadow = false
    desc.spot_shadow_mask = 0
    desc.shadowUpdate_MinRadius = SHADOW_EVERY_FRAME_RADIUS
    return desc
end

-- Muted in place rather than freed: the game owns the entity and its slot, and reloading the slot from Properties gives
-- its light back exactly.
local function MuteFakeLight(fakeLight)
    local desc = FakeLightDesc(fakeLight, FAKE_LIGHT_SLOT)
    desc.diffuse_color = { x = 0, y = 0, z = 0 }
    desc.specular_multiplier = 0
    desc.cast_shadow = 0
    fakeLight:LoadLight(FAKE_LIGHT_SLOT, desc)
end

local function UnmuteFakeLight(fakeLight)
    fakeLight:LoadLight(FAKE_LIGHT_SLOT, FakeLightDesc(fakeLight, FAKE_LIGHT_SLOT))
end

function TrueLight:RemoveFlameLight()
    local item = self.flameItemId and System.GetEntity(self.flameItemId)
    if item and item.trueLightSlot then
        item:FreeSlot(item.trueLightSlot)
        item.trueLightSlot = nil
    end
    self.flameItemId = nil
end

-- Built like the light Torch.lua gives a dropped torch while it burns out.
function TrueLight:LightFlame(item, heldLight, fakeLight)
    self:RemoveFlameLight()
    local helper = FlameHelper(item, heldLight)
    item.trueLightSlot = item:LoadLight(-1, AllDirectionsDesc(fakeLight))
    item:SetSlotPosAndDir(item.trueLightSlot, item:GetSlotHelperPos(0, helper, true), item:GetHelperDir(helper, true))
    self.flameItemId = item.id
end

function TrueLight:IsRaised(item, heldLight, now)
    if item.id ~= self.raiseItemId then
        self.raiseItemId, self.raiseStartedAt, self.lastOffsetAt, self.stillFrames = item.id, now, nil, 0
    end
    local flame = FlamePosition(item, heldLight)
    local head = player:GetBonePos("Head")
    if not flame or not head then return false end
    local offsetX, offsetY, offsetZ = flame.x - head.x, flame.y - head.y, flame.z - head.z
    if LengthSquared(offsetX, offsetY, offsetZ) > HELD_MAX_DISTANCE * HELD_MAX_DISTANCE then
        self.lastOffsetAt = nil
        return false
    end
    if self.lastOffsetAt and now > self.lastOffsetAt then
        local moved = math.sqrt(LengthSquared(offsetX - self.lastOffsetX, offsetY - self.lastOffsetY,
            offsetZ - self.lastOffsetZ))
        local speed = moved / (now - self.lastOffsetAt)
        self.stillFrames = speed < SETTLED_MAX_SPEED and self.stillFrames + 1 or 0
    end
    self.lastOffsetX, self.lastOffsetY, self.lastOffsetZ, self.lastOffsetAt = offsetX, offsetY, offsetZ, now
    return offsetZ >= RAISED_MIN_HEIGHT
        or self.stillFrames >= SETTLED_FRAMES
        or now - self.raiseStartedAt >= HANDOVER_MAX_DELAY
end

-- While the torch model is despawned (crouching, mounting) there is no flame, so the fake light's entity carries the
-- light where the game puts it, without the cone.
function TrueLight:SetGapLight(fakeLight, enabled)
    if enabled and not self.gapSlot then
        self.gapSlot = fakeLight:LoadLight(-1, AllDirectionsDesc(fakeLight))
    elseif not enabled and self.gapSlot then
        fakeLight:FreeSlot(self.gapSlot)
        self.gapSlot = nil
    end
end

function TrueLight:Restore()
    self:RemoveFlameLight()
    local fakeLight = self.fakeLightId and System.GetEntity(self.fakeLightId)
    if fakeLight then
        self:SetGapLight(fakeLight, false)
        if self.fakeLightMuted then UnmuteFakeLight(fakeLight) end
    end
    self.fakeLightId, self.fakeLightMuted, self.gapSlot = nil, false, nil
end

-- immediate: entering or leaving photo mode, where no later tick would finish the raise check.
function TrueLight:Update(immediate)
    local fakeLight = System.GetEntityByName(FAKE_LIGHT_NAME)
    if not fakeLight or not self:WantsRealLight() then
        if self.fakeLightId then self:Restore() end
        return
    end
    if fakeLight.id ~= self.fakeLightId then
        -- A new torch was lit: the old fake light took our changes with it.
        self:Restore()
        self.fakeLightId = fakeLight.id
    end
    local now = System.GetCurrTime()
    if not self.fakeLightMuted or now - self.mutedAt > REMUTE_INTERVAL then
        MuteFakeLight(fakeLight)
        self.fakeLightMuted, self.mutedAt = true, now
    end
    local item, heldLight = FindHeldLight(self.heldLights)
    if item and (item.id == self.flameItemId or immediate or self:IsRaised(item, heldLight, now)) then
        if item.id ~= self.flameItemId then self:LightFlame(item, heldLight, fakeLight) end
        self:SetGapLight(fakeLight, false)
    else
        self:RemoveFlameLight()
        self:SetGapLight(fakeLight, true)
    end
end

function TrueLight:Start()
    self.generation = (self.generation or 0) + 1
    self.updateErrorLogged = false
    local generation = self.generation
    local function Tick()
        -- A chain from before a Stop or a rerun ends here.
        if generation ~= TrueLight.generation then return end
        if TrueLight.photoMode and System.GetCurrAsyncTime() - TrueLight.photoModeOpenedAt > PHOTO_MODE_GRACE then
            TrueLight.photoMode = false
        end
        local updated, failure = pcall(TrueLight.Update, TrueLight)
        if not updated and not TrueLight.updateErrorLogged then
            TrueLight.updateErrorLogged = true
            Log("update error: " .. tostring(failure))
        end
        Script.SetTimer(1, Tick)
    end
    Script.SetTimer(1, Tick)
end

function TrueLight:Stop()
    self.generation = (self.generation or 0) + 1
    pcall(self.Restore, self)
end

function TrueLight:OnGameStart()
    -- Again here in case the Player script loaded after this file.
    HookPlayerActions()
    self.photoMode = false
    self:LoadCfg()
    self:Start()
end

function TrueLight:EventListener(actionName, eventName)
    if actionName == "System" and eventName == "OnGameplayStarted" then
        pcall(self.OnGameStart, self)
    end
end

-- HotPak calls this before it reruns the file.
function TrueLight:OnHotReload()
    self:Stop()
end

-- Any other rerun (the live Lua runner) gets the same teardown.
if TrueLight.started then TrueLight:OnHotReload() end
if not TrueLight.cfg then TrueLight:ApplyDefaults() end

if not TrueLight.eventRegistered and UIAction then
    UIAction.RegisterEventSystemListener(TrueLight, "", "", "EventListener")
    TrueLight.eventRegistered = true
end

for _, setting in ipairs(SETTINGS) do
    System.AddCCommand(CMD_PREFIX .. setting.key, "TrueLight.Set." .. setting.key .. "(%line)", "True Light: " .. setting.help)
end
System.AddCCommand(CMD_PREFIX .. "held_light", "TrueLight:CmdHeldLight(%line)",
    "True Light: count another item as a held light: <class | name:prefix> [helper | -] [state | -]; no value lists them")
System.AddCCommand(CMD_PREFIX .. "reload", "TrueLight:LoadCfg()", "True Light: read " .. CFG_PATH .. " again")

HookPlayerActions()
TrueLight.started = true
-- Loaded into a running game: OnGameplayStarted has already fired.
if player then TrueLight:OnGameStart() end
Log("loaded v" .. TrueLight.Version)
