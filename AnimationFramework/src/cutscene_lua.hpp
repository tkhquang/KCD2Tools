/**
 * @file cutscene_lua.hpp
 * @brief The Lua side of AnimationFramework.Cutscene, run once through the game's script system.
 *
 * Play(def) starts one scene through the game's own cutscene player:
 *  - the scene layer (streamed) and a profile over it are registered with animfw_scene_layer / animfw_scene_profile;
 *  - a CutsceneHolder and an IngameCutsceneData are spawned, made linkable and linked ("cutsceneData");
 *  - a scene whose cast is spawned at run time (def.cast) is placed around an anchor (the player, or given), joins the
 *    layer while visible and is then hidden, so the game shows it when the cutscene starts and removes it at the end;
 *  - def.sequenceFile is loaded the next frame, its keys moved to the anchor, so node names find the spawned cast;
 *  - def.soundBank's cutscene sound banks load, for the cutscene's Sound keys;
 *  - the holder is enqueued; when it leaves animfw_cutscene_playing, the holder and data go, the banks unload and
 *    def.onEnd(def, played) runs.
 * A scene whose layer file holds the cast and a SequenceObject (the game's own cutscene form) needs only name, row,
 * layer and sequence (layerId comes from the layer name unless given).
 */
#ifndef ANIMFW_CUTSCENE_LUA_HPP
#define ANIMFW_CUTSCENE_LUA_HPP

namespace AnimationFramework
{
    inline constexpr const char *CUTSCENE_LUA_CHUNK = "AnimationFramework.Cutscene";

    inline constexpr const char CUTSCENE_LUA[] = R"lua(
AnimationFramework = AnimationFramework or {}
local C = AnimationFramework.Cutscene or {}
AnimationFramework.Cutscene = C

local function cmd(text)
  System.ExecuteCommand(text)
end

local function remove(name)
  local e = System.GetEntityByName(name)
  if e then System.RemoveEntity(e.id) end
end

-- Heading of the player as a yaw in degrees (forward +Y at 0, counter-clockwise).
local function player_anchor()
  local p = player:GetWorldPos()
  local d = player:GetDirectionVector()
  return { x = p.x, y = p.y, z = p.z, yaw = math.deg(math.atan2(-d.x, d.y)) }
end

local function place(anchor, pos)
  pos = pos or {}
  local x, y, z = pos.x or pos[1] or 0, pos.y or pos[2] or 0, pos.z or pos[3] or 0
  if not anchor then return { x = x, y = y, z = z } end
  local r = math.rad(anchor.yaw or 0)
  local c, s = math.cos(r), math.sin(r)
  return { x = anchor.x + x * c - y * s, y = anchor.y + x * s + y * c, z = anchor.z + z }
end

C.scenes = C.scenes or {}

-- A layer's runtime id from its name: 40000..64999, above every id the game's levels use (Kutna Hora's top is 3837).
local function layer_id(name)
  local h = 0
  for i = 1, #name do h = (h * 31 + string.byte(name, i)) % 25000 end
  return 40000 + h
end
-- The scenes playing now, by sequence name, for the events their Event nodes fire.
C._active = C._active or {}

-- Gives the player one item of a class and takes it back 2 s later: a quest waits for it with an
-- ItemDescriptorTrigger on the player's inventory.
local function signal(item)
  pcall(function() player.inventory:CreateItem(item, 1, 1) end)
  Script.SetTimer(2000, function()
    pcall(function() player.inventory:DeleteItemOfClass(item, 1) end)
  end)
end

-- Polls of animfw_cutscene_playing (50 ms apart, the first 0.5 s after the start) before a scene that never showed
-- up counts as refused by the game's player.
local START_POLLS = 10

-- Whether the game's cutscene player runs the cutscene of this holder now.
local function playing(holder)
  for word in string.gmatch(System.GetCVar("animfw_cutscene_playing") or "", "%S+") do
    if word == holder then return true end
  end
  return false
end

-- The scene that holds def's holder or layer now: the game's player runs it, or it was enqueued and has not started
-- yet. The player keeps the holder, its data and the layer's entities until the cutscene ends, and deleting them under
-- it crashes the game, so Play waits for that scene.
local function busy_with(def, holder)
  for _, other in pairs(C._active) do
    if other.holder == holder or other.def.layer == def.layer then
      local queued = not other.started and other.polls <= START_POLLS and System.GetEntityByName(other.holder)
      if playing(other.holder) or queued then
        return other
      end
    end
  end
  return nil
end

-- def.soundBank = name or { names }: the game's cutscene banks the scene's Sound keys start events from. The game loads
-- a cutscene's bank only when a cutscene of that name starts, so a renamed copy or a new scene loads them itself, for
-- as long as any scene using them plays.
C._banks = C._banks or {}

local function banks_of(def)
  local names = type(def.soundBank) == "table" and def.soundBank or { def.soundBank }
  local banks = {}
  for _, name in ipairs(names) do
    if type(name) == "string" and name ~= "" then
      banks[#banks + 1] = name
      banks[#banks + 1] = name .. ".assets"
    end
  end
  return banks
end

local function hold_banks(def)
  for _, bank in ipairs(banks_of(def)) do
    C._banks[bank] = (C._banks[bank] or 0) + 1
    if C._banks[bank] == 1 then
      cmd("wh_snd_loadbankmetadata " .. bank)
      cmd("wh_snd_loadbank " .. bank)
    end
  end
end

local function release_banks(def)
  for _, bank in ipairs(banks_of(def)) do
    local count = (C._banks[bank] or 1) - 1
    C._banks[bank] = count > 0 and count or nil
    if count <= 0 then
      cmd("wh_snd_unloadbank " .. bank)
      cmd("wh_snd_unloadbankmetadata " .. bank)
    end
  end
end

-- Called by the framework for each key of an Event node (the game's TriggerTrackEvent) while its scenes play.
function C._TrackEvent(sequence, event, param)
  local state = C._active[sequence]
  if not state then return end
  local def = state.def
  if type(def.signals) == "table" and def.signals[event] then signal(def.signals[event]) end
  if type(def.onEvent) == "function" then pcall(def.onEvent, def, event, param) end
end

-- Register(def) keeps a scene under def.name, so Play("name") starts it: what a quest's ExecuteConsoleString runs as
-- `#AnimationFramework.Cutscene.Play('name')`.
function C.Register(def)
  if type(def) ~= "table" or type(def.name) ~= "string" then
    System.LogAlways("[AnimationFramework] Cutscene.Register needs a table with a name")
    return false
  end
  C.scenes[def.name] = def
  return true
end

function C.Play(def)
  if type(def) == "string" then
    local found = C.scenes[def]
    if not found then
      System.LogAlways("[AnimationFramework] Cutscene.Play: no scene registered as '" .. def .. "'")
      return nil
    end
    def = found
  end
  if type(def) ~= "table" or not def.name or not def.row or not def.layer or not def.sequence then
    System.LogAlways("[AnimationFramework] Cutscene.Play needs name, row, layer and sequence")
    return nil
  end
  def.layerId = def.layerId or layer_id(def.layer)
  -- def.speakers = { [voice] = entity name or list }: with SoundFramework, those mouths follow the voice line when the
  -- scene's Voice track plays it (a mod's own recordings; the game's lines keep their own faces).
  if type(def.speakers) == "table" then
    for voice, who in pairs(def.speakers) do
      local names = type(who) == "table" and table.concat(who, " ") or tostring(who)
      cmd(string.format("sfw_cutscene_actors %s %s", voice, names))
    end
  end
  local profile = def.profile or (string.sub(def.layer, 1, 7) == "animfw_" and def.layer or ("animfw_" .. def.layer))
  local holder = "animfw_holder_" .. def.name
  local data = "animfw_data_" .. def.name
  local other = busy_with(def, holder)
  if other then
    System.LogAlways("[AnimationFramework] Cutscene.Play: '" .. def.name .. "' does not start while '" ..
      other.def.name .. "' plays")
    return nil
  end
  remove(holder)
  remove(data)
  cmd(string.format("animfw_scene_layer %s %d", def.layer, def.layerId))
  cmd(string.format("animfw_scene_profile %s %s", profile, def.layer))

  local anchor = nil
  if def.anchor == "player" then
    anchor = player_anchor()
  elseif type(def.anchor) == "table" then
    anchor = def.anchor
  end
  local here = player:GetWorldPos()
  local d = System.SpawnEntity({ class = "IngameCutsceneData", name = data, position = here,
    properties = { esGameProfile = profile, esSequenceName = def.sequence } })
  local h = System.SpawnEntity({ class = "CutsceneHolder", name = holder, position = here,
    properties = { esCutsceneName = def.row } })
  if not d or not h then
    System.LogAlways("[AnimationFramework] Cutscene.Play: the holder or data entity did not spawn")
    remove(holder)
    remove(data)
    return nil
  end

  -- def.teleport = {x, y, z}: where the player stands when the cutscene ends (vanilla quests move the player
  -- themselves; the cutscene player does not read the holder's "teleport" link).
  local teleport = nil
  if type(def.teleport) == "table" then
    teleport = place(nil, def.teleport)
  end

  local cast = {}
  for _, member in ipairs(def.cast or {}) do
    remove(member.name)
    local e = System.SpawnEntity({ class = member.class, name = member.name, position = place(anchor, member.pos),
      properties = member.properties })
    if e then
      e:SetWorldAngles({ x = 0, y = 0, z = math.rad((anchor and anchor.yaw or 0) + (member.yaw or 0)) })
      cast[#cast + 1] = e
    end
  end
  if #cast > 0 then
    local names = {}
    for _, e in ipairs(cast) do names[#names + 1] = e:GetName() end
    -- Joining the layer while visible lets the layer show the member when the cutscene starts.
    cmd(string.format("animfw_scene_layer %s %d %s", def.layer, def.layerId, table.concat(names, " ")))
    for _, e in ipairs(cast) do e:Hide(1) end
  end

  local state = { def = def, holder = holder, data = data, cast = cast, started = false, polls = 0 }
  C._active[def.sequence] = state
  hold_banks(def)
  local function finish()
    remove(holder)
    remove(data)
    release_banks(def)
    if teleport and state.started then player:SetWorldPos(teleport) end
    for _, e in ipairs(cast) do
      if System.GetEntity(e.id) then System.RemoveEntity(e.id) end
    end
    if C._active[def.sequence] == state then C._active[def.sequence] = nil end
    -- def.signalItem: an item class the player gets when the scene has played (see signal).
    if def.signalItem and state.started then signal(def.signalItem) end
    if type(def.onEnd) == "function" then pcall(def.onEnd, def, state.started) end
  end
  -- No time limit while the holder plays: removing the holder or its data under a running cutscene crashes the game.
  local function poll()
    state.polls = state.polls + 1
    local mine = playing(holder)
    if mine then state.started = true end
    if (state.started and not mine) or (not state.started and state.polls > START_POLLS) then
      if not state.started then
        System.LogAlways("[AnimationFramework] Cutscene.Play: '" .. def.name .. "' did not start")
      end
      finish()
    else
      Script.SetTimer(50, poll)
    end
  end
  Script.SetTimer(100, function()
    if def.sequenceFile then
      if anchor then
        cmd(string.format("animfw_load_sequence %s @ %.3f %.3f %.3f %.3f", def.sequenceFile, anchor.x, anchor.y,
          anchor.z, anchor.yaw or 0))
      else
        cmd(string.format("animfw_load_sequence %s", def.sequenceFile))
      end
    end
    cmd(string.format("animfw_make_linkable %s %s", holder, data))
    if not XGenAIModule.AddLink(XGenAIModule.GetMyWUID(h), XGenAIModule.GetMyWUID(d), "cutsceneData") then
      System.LogAlways("[AnimationFramework] Cutscene.Play: the holder could not link its data")
    end
    cmd(string.format("animfw_play_cutscene %s", holder))
    Script.SetTimer(500, poll)
  end)
  return state
end
)lua";
} // namespace AnimationFramework

#endif // ANIMFW_CUTSCENE_LUA_HPP
