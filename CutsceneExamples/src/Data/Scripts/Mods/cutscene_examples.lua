-- Cutscene Examples: Animation Framework cutscene examples.
-- Copyright (c) 2026-present Quang Trinh (tkhquang) <khacquang.trinh@gmail.com>
-- https://tkhquang.dev - MIT License
--
-- Magdalena in Kutna Hora offers one topic per example (Quests/cutscene_examples). Once the conversation closes, the
-- quest runs `#CutsceneExamples.Play('<example>')`, which plays it through AnimationFramework.Cutscene:
--  - dance: a scene made from scratch (dance_battle_stage, written by a generator): mod animations, a crowd of the
--    game's townsfolk, the game's crowd sounds, an end card. Henry wins or loses at random.
--  - bergow: a copy of one of the game's own scenes (animfw clone-cutscene), the fencers' bout in Kutna Hora: Henry's
--    looks on the winner, Jobst von Bergow's on the loser and Hans's on the fencing master, cut as the loser goes down.
--  - duel: the same bout copied to an open field (animfw clone-cutscene --at) with only its two fighters, now Henry and
--    Hans in his armour.
-- The scenes clear the people around them as the game's own cutscenes do (a SequenceArea in their CleanupAreas).

CutsceneExamples = CutsceneExamples or {}
local M = CutsceneExamples

local function log(text)
  System.LogAlways("[CutsceneExamples] " .. text)
end

-- The game's cutscene sound banks the scenes' sound keys come from. The fencers' bout bank holds the bout's own effects
-- and a town crowd murmuring and clapping along, which the dance scenes use too. The dance scenes add peasants cheering
-- and laughing, and the lose scene a crowd laughing at a fall.
local FENCERS_BOUT = "cin_s3970k_sermiri__fencers_fight"
local CHEERS = "cin_m0510t_svatba__wedding_ceremony"
local LAUGHS = "cin_m1210t_vezninatroskach__prisoners_convoy"

local function scene(name, layer, banks)
  return {
    name = name, row = name, layer = layer, sequence = name, soundBank = banks,
    onEnd = function(def, played) log(name .. " ended, played " .. tostring(played)) end,
  }
end

M.SCENES = {
  dance_win = scene("dance_battle_win", "dance_battle_stage", { FENCERS_BOUT, CHEERS }),
  dance_lose = scene("dance_battle_lose", "dance_battle_stage", { FENCERS_BOUT, CHEERS, LAUGHS }),
  -- A copy keeps the original's sound bank (clone-cutscene prints it in its Register line).
  bergow = scene("ce_bergow_bout", "ce_bergow_bout", FENCERS_BOUT),
  duel = scene("ce_field_duel", "ce_field_duel", FENCERS_BOUT),
}

-- Plays one example: 'dance' (rolls the outcome 50/50), 'bergow' or 'duel'.
function M.Play(example)
  local cutscene = AnimationFramework and AnimationFramework.Cutscene
  if not cutscene then
    log("the Animation Framework is not installed or not loaded")
    return
  end
  local key = example
  if example == "dance" then
    key = math.random(2) == 1 and "dance_win" or "dance_lose"
  end
  local def = M.SCENES[key]
  if not def then
    log("no example named '" .. tostring(example) .. "'")
    return
  end
  log("playing " .. key)
  cutscene.Play(def)
end
