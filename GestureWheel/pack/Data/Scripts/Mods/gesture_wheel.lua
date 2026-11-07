-- Gesture Wheel: hold the key to open the wheel, point at a slot, release to act. The movie
-- (Libs/UI/KCD2_GestureWheel.gfx) reads the pointer and sends events; this script owns what the slots mean. Henry plays
-- the slot's fragment and says one of his own lines from the game's voice files, and the NPC he faces answers: its own
-- animation (through the Animation Framework, which opens human:PlayAnim to NPCs), a voiced line and a change in how it
-- regards him.
GestureWheel = GestureWheel or {}
local Wheel = GestureWheel

local ELEMENT = "KCD2_GestureWheel"
local INSTANCE = 0
local PROFILE = "Data/Libs/Config/KCD2_GestureWheel_Profile.xml"
local ACTION_MAP = "gesture_wheel"
local OPEN_ACTION_MAP = "gesture_wheel_open"
-- Always on: the right stick's directions, which pick a slot while the wheel is open (see Wheel.PadStick).
local PAD_ACTION_MAP = "gesture_wheel_pad"
local CENTER_LABEL = "E: edit slot"
local NO_SLOT = -1
-- The slot a gamepad stick direction points at: slot 0 at the top, clockwise.
local PAD_SLOTS = { up = 0, ["up+right"] = 1, right = 2, ["down+right"] = 3, down = 4, ["down+left"] = 5, left = 6, ["up+left"] = 7 }
-- How long a stick direction turning off waits before the slot follows, in milliseconds (see Wheel.PadStick).
local PAD_SETTLE_MS = 80
-- The NPC that answers is the nearest one within this range (meters) and cone in front of the camera.
local TARGET_RANGE = 6
local TARGET_MIN_DOT = 0.6
-- How long after Henry starts a gesture the NPC answers, in milliseconds.
local ANSWER_DELAY_MS = 900
-- An NPC's walking fragment, and the speed (m/s) above which an NPC counts as moving: walkers measured 0.85-1.55.
local WALKING_STATE = "MotionMovement"
local MOVING_SPEED = 0.3
-- One NPC changes its regard for Henry at most once in this many seconds.
local REPUTATION_COOLDOWN_S = 60
-- The Threatened reaction fights at this many threats in a row, each within this many seconds of the last.
local THREATS_TO_FIGHT = 3
local THREAT_WINDOW_S = 30
local BARK_MESSAGE = "crime:barkRequest"
local ATTACK_MESSAGE = "crime:attackInitiatedByConcept"
-- Tells PlayAnim to play a fragment for part of the body on its own scopes, not over the whole body.
local OWN_SCOPES_TAG = "animfw_own_scopes"
-- Tells PlayAnim that the tags it switches on are only for this fragment. Playing the second name takes them off again.
local TEMPORARY_TAGS_TAG = "animfw_temporary_tags"
local RESTORE_TAGS_FRAGMENT = "animfw_restore_tags"
-- Tells PlayAnim which option to play, by its number among the options with the tags it plays on (one clip of several).
local OPTION_TAG_PREFIX = "animfw_option_"
-- Tell PlayAnim to wait behind the fragment playing (the next part of a sequence), and to stay on the last pose.
local QUEUE_TAG = "animfw_queue"
local HOLD_TAG = "animfw_hold"
-- Henry's light hand (HS_LEFT) and the light the game keeps on him while he holds a lit torch or lamp.
local LIGHT_HAND = 1
local TORCH_LIGHT = "PlayerAuxTorchLight"
local showTorch
-- What a gesture does with Henry's hands: take out his melee or ranged weapon set or a torch for it, or empty them; none
-- leaves them to the animation. His weapon hand (HS_RIGHT), the torch item class, and how long the wheel waits for the
-- weapon or torch to come out (or go away) before the gesture plays anyway.
local HANDS = { melee = true, ranged = true, torch = true, empty = true }
local WEAPON_HAND = 0
local TORCH_CLASS = "4cea28a0-0814-405a-bf24-4fd711f7eb63"
local HANDS_WAIT_MS = 2500
-- A prop is a model put in one of Henry's hands while a gesture plays (a fishing rod, a lantern, a mug, any weapon): an
-- entity of PROP_CLASS that only shows the model, attached to the hand as the game attaches what an NPC holds, and
-- removed when the gesture ends. What the hand held goes into the bag meanwhile and comes back after. The hands by
-- name, and each prop entity's name (a prop left over from a reload or a load is removed by it).
local PROP_HANDS = { right = 0, left = 1 }
-- The other place a prop can go: in front of Henry, attached to him where the game puts the book he reads from his
-- inventory (its MotionIdle readingFromInventory poses hold it there). The game sets that spot in its code, one for
-- each posture, so they are measured in game; the book is tilted 45 degrees toward him in all of them.
local PROP_FRONT = "front"
local FRONT_PROP_POS = {
	stand = { x = 0, y = 0.2, z = 1.35 },
	seat = { x = 0, y = 0.31, z = 0.9 },
	horse = { x = 0, y = 0.27, z = 0.6 },
}
local FRONT_PROP_ANGLES = { x = -math.pi / 4, y = 0, z = 0 }
local PROP_PLACES = { "right", "left", PROP_FRONT }
local PROP_CLASS = "BasicEntity"
local PROP_NAME = "gesture_wheel_prop_"
-- A prop with this model only empties its hand while the gesture plays.
Wheel.EMPTY_HAND = "-"
-- Blend into a prop's own clip, in seconds.
local PROP_BLEND_S = 0.1
-- Henry's weapon sets, and the class of a drawn bow's or crossbow's prop (a melee weapon's is PickableItem).
local MELEE_SET = WS_PRIMARY or 0
local RANGED_SET = WS_SECONDARY or 1
local RANGED_CLASS = "MissileWeapon"
-- How long a torch put back in Henry's hand may take to show there before it is put back again, in milliseconds.
local LIGHT_REDRAW_MS = 150
-- How long after the weapon shows drawn its draw animation still plays, which the gesture would cut.
local DRAW_SETTLE_MS = 300
-- An option the player chose is in this set when it is one of the fragment's standing options.
local STAND_SET = "stand"
-- An option the player chose is in these sets when it is the fragment's version for a seat or a horse.
local SEAT_SET = "seat"
local HORSE_SET = "horse"
-- What a seat or a horse falls back to when none of a gesture's animations plays there, by the gesture's reaction: stock
-- conversation fragments with options standing, on both kinds of seat and on a horse, which Henry's own tags pick.
local FALLBACK_ANIMATIONS = {
	friendly = { fragment = "ADLG_Agree", seat = "", horse = "" },
	annoyed = { fragment = "ADLG_Disagree", seat = "", horse = "" },
	threatened = { fragment = "ADLG_Emphasis", seat = "", horse = "" },
}
local FALLBACK_ANIMATION = { fragment = "ADLG_Gesture", seat = "", horse = "" }
-- How often a held fragment is checked, and how long it may take to start, in milliseconds.
local HOLD_CHECK_MS = 100
local HOLD_START_MS = 1000
-- A standing full-body gesture ends when Henry's position moves faster than SLIDE_SPEED (m/s) over the last
-- SLIDE_WINDOW_S seconds, or drops faster than FALL_SPEED (see watchSlide). Walking is 0.85-1.55; the gestures' own
-- movement peaks at 0.55 (Ground Sit's sit-down; every other built-in gesture keeps him in place).
local SLIDE_SPEED = 0.75
local SLIDE_WINDOW_S = 0.1
local FALL_SPEED = 2
-- Added to the length of a part Henry's state never shows, in seconds, before the gesture counts it as over: about one
-- check, so the torch shows as the blend out starts.
local LENGTH_GRACE_S = 0.05
-- A looping fragment never ends by itself, so a locked one without a duration is stopped after this long.
local HOLD_LIMIT_S = 15
-- A locked gesture (lock) holds Henry in its full-body animation with this map: the game's map for moments the player
-- only looks, with the camera and the pause menu, no movement and no other menu. Otherwise nothing holds him, so moving
-- ends the gesture and every other key works as usual, the inventory too (the game stops while it is open, and the
-- gesture with it). The game's cart map would keep the menus with Henry held, but shows a Dismount prompt.
local STILL_ACTION_MAP = "noninteractive"
-- On while a gesture plays: its keys (movement, left stick, jump) end the gesture through these commands.
local STOP_ACTION_MAP = "gesture_wheel_playing"
local STOP_COMMANDS = {
	"gesturewheel_stop_forward", "gesturewheel_stop_back", "gesturewheel_stop_left", "gesturewheel_stop_right",
	"gesturewheel_stop_jump",
}
-- Always on, unlike the stop map: the left stick's two axes, each a press on any change past the dead zone.
local STICK_ACTION_MAP = "gesture_wheel_stick"
local STICK_AXES = { "x", "y" }
-- Henry's collider while a gesture plays (EColliderMode Disabled), and no request from script, which gives the game its
-- own mode back.
local COLLIDER_DISABLED = 1
local COLLIDER_RESTORED = 0
-- Plays a voice file as speech on Henry. The command needs the file's path in the pak, without its extension.
local SPEAK_COMMAND = "wh_snd_playfile Localization/dialog/"

-- Henry's lines, from the voice files. Their names start with tmck_, his actor; the text is in text_ui_dialog.xml.
local VOICES = {
	wave = {
		"kutnohorsko/hledanilichtenstejna/tmck_neme_henry_zdravim__FIc1", -- Greetings!
		"open_world/bff_pozdravy_a_rozlouceni/bff_zdravi_hrace_chatem__samuel/tmck_samu_henry_zdravicko__x5Xg", -- Greetings!
		"open_world/bff_pozdravy_a_rozlouceni/tmck_bff__hrac_bud_zdrav_hansi_LoOe", -- Greetings!
		"random_events/pocestny/tmck_hada_henry_pozdrav_panbuh__7CUw", -- Greetings!
		"random_events/prepadeninaceste/tmck_chat_henry_bud_zdrav_J9iM", -- Greetings!
		"trosecko/socky/tmck_dial_henry_co_vis_o_ty_ve_uM7M", -- Greetings!
		"trosecko/svatba/tmck_jind_henry_zdravim_oz2u", -- Greetings!
		"open_world/bff_pozdravy_a_rozlouceni/bff_zdravi_hrace_chatem__musa/tmck_bff__hrac_dobryho_dne_pta_E5bm", -- Good day!
		"random_events/taboryUCesty/tmck_chat_henry_pozdrav_panbuh_pUzE", -- Good day!
		"trosecko/socky/tmck_dial_henry_prej_tady_v_kr_Ihcr", -- Good day!
		"open_world/reakce_na_stav_hrace/npc_zdravi_hrace_chatem__straz/tmck_npc__henry_bud_zdrav_KYdF", -- Greetings.
		"trosecko/socky/tmck_tria_henry_pozdrav_panbuh_GRIi", -- Greetings to you.
	},
	flute = {
		"trosecko/svatba/tmck_vyzv_henry_hudbo_hraj_usEx", -- Let there be music!
	},
	ground_sit = {
		"open_world/stavy_hrace_unavahlad/tmck_jind_hrac_uz_bych_to_zalo_Z1ZQ", -- I feel like laying down.
		"open_world/stavy_hrace_unavahlad/tmck_jind_hrac_uz_padam_unavou_Si5u", -- I'm exhausted.
		"open_world/stavy_hrace_unavahlad/tmck_jind_hrac_ach_jo_sel_bych_FNU4", -- Ugh, I'm getting quite tired.
		"open_world/stavy_hrace_unavahlad/tmck_jind_hrac_zdrimnul_bych_si_aTiJ", -- I could lay down for a bit.
		"kutnohorsko/hledanilichtenstejna/tmck_kate_henry_hm_NA5q", -- Hmm.
	},
	laugh = {
		"open_world/vozy_a_karavany/tmck_jind_henry_hehe_gHGi", -- Ha ha...
		"trosecko/mlynaruvUcen/tmck_mlyn_henry_ha_ha_ha__sFJZ", -- Ha ha ha!
		"kutnohorsko/listovniTajemstvi/tmck_psan_henry_ha_ha_ha_ha_S9va", -- Ha ha ha ha.
		"trosecko/pracharna/tmck_zink_henry_haha_Sq6M", -- Haha!
		"trosecko/vezniNaTroskach/tmck_cust_henry_hahaha_x7Jj", -- Hahaha...
	},
	rude = {
		"utils/punishment/tmck_stra_hrac_trhni_si_0utN", -- Piss off!
		"kutnohorsko/stealthMiseZaJindru/tmck_post_henry_vypadni_wGMf", -- Get lost!
		"utils/punishment/tmck_stra_hrac_naser_si_AVHt", -- Fuck off!
	},
	threaten = {
		"kutnohorsko/stealthMiseZaJindru/tmck_dial_henry_bez_mi_z_cesty_Fw8F", -- Get out of my way!
		"kutnohorsko/zakopanyZitrek/tmck_hank_henry_ani_se_nehni_neS3", -- Don't move a muscle!
		"kutnohorsko/kralovskeStribro/tmck_dial_henry_nepokousej_moj_GWBy", -- Don't try my patience, fellah!
		"kutnohorsko/prepadeniVlasskehoDvora/jindra__vlassky_dvur_ow/osvobozeni_panu/tmck_csab_henry_seru_na_tebe_c_3y9Q", -- Fuck you! Die!
		"kutnohorsko/drak/tmck_tria_henry_taste_3qkb", -- Draw your steel, fuckers!
		"kutnohorsko/drak/tmck_tria_henry_bat_se_nafuky__7PPV", -- I eat cocky bastards like you for breakfast!
		"kutnohorsko/utokNaMalesov/tmck_nepr_henry_s_timhle_buh_n_xM6l", -- Fuck you!
		"kutnohorsko/damaVNesnazich/tmck_tria_henry_naval_mi_ty_pr_qqjt", -- Hand me that money, you bitch!
	},
}

-- What a woman plays when none of a reaction's animations is in her database, by whether the reaction raises or lowers
-- Henry's standing. Both databases have these, with options standing and seated, which the NPC's own tags pick.
local FALLBACK_ANSWERS = { agree = "ADLG_Agree", refuse = "ADLG_Disagree" }
local AGREE = { fragment = FALLBACK_ANSWERS.agree, women = true }
local REFUSE = { fragment = FALLBACK_ANSWERS.refuse, women = true }

-- The wheel's NPC reactions, in the form GestureWheel.RegisterReaction takes. A player can change them and add more.
-- An NPC answers with one of its animations (seated ones for a seated NPC), one of its lines and the reputation change.
-- Bark types are metaroles: the NPC says one of its own lines for it, and asked for one again repeats it, so they are
-- used in turn; a metarole needs rows in role.xml for the NPC, or it stays silent though the message is accepted.
local DEFAULT_REACTIONS = {
	{ id = "friendly", label = "Friendly", animations = { AGREE }, seated = { AGREE }, barks = { "NPC_ZDRAVI_HRACE" }, reputation = "greeting_increase", change = 0.02 },
	{ id = "annoyed", label = "Annoyed", animations = { REFUSE }, seated = { REFUSE }, barks = { "NPC_POSILA_HRACE_PRYC", "NPC_JE_SVEDKEM_VYTRZNOSTI_UNIVERZAL", "NPC_VYPROVAZI_HRACE_Z_TRESPASSU" }, reputation = "greeting_decrease", change = -0.02 },
	{ id = "threatened", label = "Threatened", animations = { REFUSE }, seated = { REFUSE }, barks = { "NPC_UKONCUJE_MINDPLAYER_HRAC_NEODESEL", "NPC_VYPROVAZI_HRACE_Z_TRESPASSU", "NPC_JE_SVEDKEM_VYTRZNOSTI_UNIVERZAL" }, reputation = "threaten_fail", change = -0.05, fight = { count = THREATS_TO_FIGHT, window = THREAT_WINDOW_S } },
	-- Quiet listeners for music: no barks or lines, so the song plays on alone.
	{ id = "audience", label = "Audience", animations = { { fragment = "ApplauseHappy" }, { fragment = "ADLG_Listen", women = true } }, seated = { { fragment = "ADLG_Agree", women = true } }, barks = {}, reputation = "greeting_increase", change = 0.02 },
}
-- How a reaction's exact voice line is spoken by the NPC, through Sound Framework, which plays it in its actor's voice.
local SAY_COMMAND = "sfw_say %s Localization/dialog/%s"
-- What GetGender gives for a woman.
local FEMALE = 2

-- The wheel's own gestures, in the form GestureWheel.Register takes (fields there). A list entry with option and clip
-- plays that one option of the fragment, with the tags it needs; seat and horse name the tags of a fragment's
-- option on a seat or a horse. Elsewhere the upper-body twin or FALLBACK_ANIMATIONS play.
local DEFAULT_GESTURES = {
	{
		id = "wave",
		label = "Wave",
		fragment = {
			{ fragment = "ADLG_Wave" },
			{ fragment = "ADLG_Bow_light" },
		},
		voice = VOICES.wave,
		reaction = "friendly",
	},
	{
		id = "sword_pose",
		label = "Sword Pose",
		intro = {
			{ fragment = "ADLG_Adjuration_sword_up" },
		},
		hold = true,
		props = { right = { model = "objects/manmade/weapons/swords_long/long_sword_henry.cgf", label = "Reforged Radzig Kobyla's sword" } },
	},
	{
		id = "flute",
		label = "Play Flute",
		fragment = {
			{ fragment = "PlayFluteSong", tags = "tempo_fast" },
		},
		voice = VOICES.flute,
		reaction = "audience",
		hold = true,
		props = { right = { model = "objects/manmade/task_specific_props/entertainment/music/flute.cgf", label = "flute" } },
	},
	{
		id = "ground_sit",
		label = "Ground Sit",
		intro = {
			{ fragment = "ADLG_Quest_MaleSittingGroundIn" },
		},
		voice = VOICES.ground_sit,
		hold = true,
	},
	{
		id = "reading",
		label = "Reading",
		fragment = {
			{ fragment = "MotionIdle", tags = "player+readingFromInventory", option = 0, clip = "holding_book_idle" },
		},
		seated = {
			{ fragment = "SittingIdle", tags = "sitting+player+reading", seat = "sitting+player+reading", option = 0, set = "seat", clip = "holding_book_sitting_idle" },
		},
		mounted = {
			{ fragment = "MotionIdle", tags = "horse+player+readingFromInventory", horse = "horse+player+readingFromInventory", option = 0, set = "horse", clip = "relaxed_idle_rider_reading" },
		},
		hold = true,
		props = { right = { model = "-", label = "Empty" }, left = { model = "-", label = "Empty" }, front = { model = "objects/characters/assets/book/book_01.cdf", label = "scribe book" } },
	},
	{
		id = "laugh",
		label = "Laugh",
		fragment = {
			{ fragment = "ADLG_Laugh", tags = "happy", option = 0, clip = "dlg_male_happy_stand_laugh_01" },
			{ fragment = "ADLG_Laugh", tags = "happy", option = 1, clip = "dlg_male_happy_stand_laugh_03" },
			{ fragment = "ADLG_Laugh", tags = "happy", option = 2, clip = "dlg_male_happy_stand_laugh_02" },
			{ fragment = "ADLG_Laugh", seat = "sittingNoTable" },
		},
		voice = VOICES.laugh,
	},
	{
		id = "rude",
		label = "Rude",
		fragment = {
			{ fragment = "ADLG_Discard", tags = "angry", option = 0, clip = "dlg_male_angry_stand_discard_02" },
			{ fragment = "ADLG_Discard", tags = "angry", option = 1, clip = "dlg_male_angry_stand_discard_03" },
			{ fragment = "ADLG_Discard", tags = "angry", option = 2, clip = "dlg_male_angry_stand_discard_01" },
			{ fragment = "ADLG_Discard", tags = "arogant", option = 0, clip = "dlg_male_arogant_stand_discard" },
			{ fragment = "ADLG_Disagree", seat = "sitting", horse = "horse" },
			{ fragment = "ADLG_Discard", seat = "sitting" },
			{ fragment = "ADLG_Dismiss", tags = "arogant" },
		},
		seated = {
			{ fragment = "ADLG_Discard", seat = "angry" },
		},
		voice = VOICES.rude,
		reaction = "annoyed",
	},
	{
		id = "threaten",
		label = "Threaten",
		fragment = {
			{ fragment = "ADLG_Threat", tags = "angry", option = 0, clip = "dlg_male_angry_stand_threaten_01" },
			{ fragment = "ADLG_Threat", tags = "angry", option = 1, clip = "dlg_male_angry_stand_threaten_02" },
			{ fragment = "ADLG_Threat", tags = "angry", option = 2, clip = "dlg_male_angry_threaten_come_at_me" },
			{ fragment = "ADLG_Threat", tags = "angry", option = 3, clip = "dlg_male_angry_threaten_matrix" },
			{ fragment = "ADLG_Threat", tags = "angry", option = 4, clip = "dlg_male_angry_stand_threaten_03" },
			{ fragment = "ADLG_Threat", tags = "arogant", option = 0, clip = "dlg_male_arogant_stand_threaten" },
			{ fragment = "Pointing" },
			{ fragment = "ADLG_Threat", seat = "sittingNoTable" },
		},
		seated = {
			{ fragment = "ADLG_Threat", seat = "angry" },
		},
		voice = VOICES.threaten,
		reaction = "threatened",
	},
}
-- The gesture in each slot, clockwise from the top.
local DEFAULT_SLOTS = { "wave", "sword_pose", "flute", "ground_sit", "reading", "laugh", "rude", "threaten" }

Wheel.isOpen = false
-- Kept through a script reload: the movie stays loaded and does not send OnReady again, so resetting it would leave the
-- wheel showing the names it had before.
Wheel.movieReady = Wheel.movieReady == true
Wheel.hovered = NO_SLOT
-- Gamepad state: RB held, and the right stick's directions that are on (see Wheel.PadStick).
Wheel.pad = { held = false, stick = {} }
-- A gesture the player built in the editor has this id, then its slot.
Wheel.CUSTOM_PREFIX = "custom_"
-- The registered gestures by id, the same in the order they came, and the id in each slot.
Wheel.gestures = {}
Wheel.gestureOrder = {}
Wheel.slots = {}
-- Gestures of mods that load before this script, which registers them when it can.
Wheel.Pending = Wheel.Pending or {}
-- Reactions of such mods, registered before their gestures.
Wheel.PendingReactions = Wheel.PendingReactions or {}
-- The NPC reactions by id, and in the order they came.
Wheel.reactions = {}
Wheel.reactionOrder = {}
-- A reaction the player built in the editor has this id, then a number.
Wheel.CUSTOM_REACTION_PREFIX = "custom_reaction_"
Wheel.threats = Wheel.threats or {}
Wheel.reputationTimes = Wheel.reputationTimes or {}
Wheel.barkTurns = Wheel.barkTurns or {}

local function log(text)
	System.LogAlways("[GestureWheel] " .. text)
end

-- Event arguments arrive in a 0-indexed table.
local function slotFrom(args)
	local value = args
	if type(args) == "table" then
		value = args[0] or args[1] or args.Slot
	end
	return tonumber(value) or NO_SLOT
end

-- An NPC animation list as { fragment, women } entries, from names or tables with those fields. women tells a fragment
-- the women's database has too; a plain name is for men only.
local function npcAnimationsOf(list)
	local animations = {}
	for _, entry in ipairs(type(list) == "table" and list or {}) do
		if type(entry) == "string" then
			entry = { fragment = entry, women = false }
		end
		if type(entry) == "table" and type(entry.fragment) == "string" then
			animations[#animations + 1] = { fragment = entry.fragment, women = entry.women and true or false }
		end
	end
	return animations
end

local function stringsOf(list)
	local strings = {}
	for _, entry in ipairs(type(list) == "table" and list or {}) do
		if type(entry) == "string" then
			strings[#strings + 1] = entry
		end
	end
	return strings
end

-- Adds an NPC reaction, or replaces the one with the same id. id and label are required. animations and seated are what
-- the NPC plays standing and seated (names or { fragment, women }); barks are metaroles and lines voice files under
-- Localization/dialog/ without the extension (spoken through Sound Framework), of which the NPC says one; reputation is
-- a row of reputation_change.xml and change its value; fight = { count, window } makes the NPC attack at count gestures
-- within window seconds of each other.
function Wheel.RegisterReaction(definition)
	if type(definition) ~= "table" or type(definition.id) ~= "string" or type(definition.label) ~= "string" then
		log("RegisterReaction: a reaction needs an id and a label")
		return false
	end
	if Wheel.reactions[definition.id] == nil then
		Wheel.reactionOrder[#Wheel.reactionOrder + 1] = definition.id
	end
	local fight = definition.fight
	Wheel.reactions[definition.id] = {
		id = definition.id,
		label = definition.label,
		animations = npcAnimationsOf(definition.animations),
		seated = npcAnimationsOf(definition.seated),
		barks = stringsOf(definition.barks),
		lines = stringsOf(definition.lines),
		reputation = type(definition.reputation) == "string" and definition.reputation ~= "" and definition.reputation or nil,
		change = tonumber(definition.change) or 0,
		fight = type(fight) == "table" and tonumber(fight.count) and { count = tonumber(fight.count), window = tonumber(fight.window) or THREAT_WINDOW_S } or nil,
		edited = definition.edited,
	}
	return true
end

-- Takes a reaction out; gestures that name it make the NPC not answer.
function Wheel.RemoveReaction(id)
	if Wheel.reactions[id] == nil then
		return
	end
	Wheel.reactions[id] = nil
	for index, known in ipairs(Wheel.reactionOrder) do
		if known == id then
			table.remove(Wheel.reactionOrder, index)
			break
		end
	end
end

-- The built-in reaction a reaction with this id would be, or nil for a custom one.
function Wheel.DefaultReaction(id)
	for _, definition in ipairs(DEFAULT_REACTIONS) do
		if definition.id == id then
			return definition
		end
	end
	return nil
end

-- A list of fragments as { fragment, partial, tags, seat, horse } entries, from names or tables with those fields. seat
-- and horse are the tags of the fragment's option for Henry on a seat or a horse ("" when it needs none), nil when it
-- has none. A plain name takes the definition's partial and tags.
local function animationsOf(list, definition)
	if type(list) ~= "table" then
		list = { list }
	end
	local animations = {}
	for _, entry in ipairs(list) do
		if type(entry) == "string" then
			entry = { fragment = entry, partial = definition.partial, tags = definition.tags }
		end
		if type(entry) == "table" and type(entry.fragment) == "string" then
			animations[#animations + 1] = {
				fragment = entry.fragment,
				partial = entry.partial,
				tags = entry.tags,
				seat = entry.seat,
				horse = entry.horse,
				option = tonumber(entry.option),
				set = entry.set,
				clip = entry.clip,
			}
		end
	end
	return animations
end

-- Adds a gesture, or replaces the one with the same id. id and label are required, and at least one animation.
-- fragment is what Henry plays standing: one fragment or a list to pick from at random, each a name or
-- { fragment, partial, tags, seat, horse }; seated and mounted are the same for a seat and a horse, and without them he
-- plays those of fragment that have an option there, else a stock fallback (FALLBACK_ANIMATIONS). voice is one voice
-- file or a list of them under Localization/dialog/, without the extension, reaction the id of a registered reaction,
-- partial marks a fragment for part of the body, tags are the ones its option needs, joined by +, slot (1 to 8) puts it
-- on the wheel, and passThrough false keeps Henry's collision while it plays (by default it is off, so touching an NPC
-- does not end it). intro, loop and outro (lists like fragment) make it a sequence: the intro, then the loop until the
-- gesture ends, then the outro. hold keeps the last animation on its last pose; duration (seconds from the start) ends
-- the loop or the hold, which otherwise last until Henry moves. hands is "melee" or "ranged" (take out that weapon set
-- for the gesture), "torch" (take out a torch) or "empty" (put the weapon away, hide the torch), each undone when the
-- gesture ends. lock true locks Henry in it: moving does not end it, the menus stay shut, and a loop or hold without a
-- duration stops after HOLD_LIMIT_S. props puts a model in a hand while it plays:
-- { right = { model = "objects/...cgf", label = "..." }, left = ..., front = ... }, any of them; what that hand held
-- comes back afterwards. A prop whose model is Wheel.EMPTY_HAND only empties the hand. front is in front of Henry,
-- where the game holds the book he reads.
function Wheel.Register(definition)
	if type(definition) ~= "table" or type(definition.id) ~= "string" or type(definition.label) ~= "string" then
		log("Register: a gesture needs an id and a label")
		return false
	end
	local animations = animationsOf(definition.fragment, definition)
	local seated = animationsOf(definition.seated, definition)
	local mounted = animationsOf(definition.mounted, definition)
	local intro = animationsOf(definition.intro, definition)
	local loop = animationsOf(definition.loop, definition)
	local outro = animationsOf(definition.outro, definition)
	if #animations + #seated + #mounted + #intro + #loop + #outro == 0 then
		log("Register: " .. definition.id .. " needs a fragment")
		return false
	end
	local duration = tonumber(definition.duration)
	-- A reaction registered later still works; one that never comes makes the NPC not answer.
	if definition.reaction ~= nil and Wheel.reactions[definition.reaction] == nil then
		log("Register: " .. definition.id .. " names a reaction not registered yet: " .. tostring(definition.reaction))
	end
	local voice = definition.voice
	if type(voice) == "string" then
		voice = { voice }
	end
	local props
	for _, hand in ipairs(PROP_PLACES) do
		local prop = type(definition.props) == "table" and definition.props[hand]
		if type(prop) == "table" and type(prop.model) == "string" and prop.model ~= "" and not (hand == PROP_FRONT and prop.model == Wheel.EMPTY_HAND) then
			props = props or {}
			props[hand] = { model = prop.model, label = type(prop.label) == "string" and prop.label or prop.model }
		end
	end
	if Wheel.gestures[definition.id] == nil then
		Wheel.gestureOrder[#Wheel.gestureOrder + 1] = definition.id
	end
	Wheel.gestures[definition.id] = {
		id = definition.id,
		label = definition.label,
		animations = animations,
		seated = seated,
		mounted = mounted,
		intro = intro,
		loop = loop,
		outro = outro,
		hold = definition.hold == true or nil,
		duration = duration and duration > 0 and duration or nil,
		hands = HANDS[definition.hands] and definition.hands or nil,
		props = props,
		last = {},
		voice = voice or {},
		reaction = definition.reaction,
		passThrough = definition.passThrough ~= false,
		lock = definition.lock == true or nil,
	}
	local slot = definition.slot
	if slot ~= nil then
		if Wheel.slots[slot] == nil then
			log("Register: " .. definition.id .. " has no slot " .. tostring(slot))
		else
			Wheel.slots[slot] = definition.id
		end
	end
	return true
end

local function registerPending()
	for index, definition in ipairs(Wheel.PendingReactions) do
		Wheel.RegisterReaction(definition)
		Wheel.PendingReactions[index] = nil
	end
	for index, definition in ipairs(Wheel.Pending) do
		Wheel.Register(definition)
		Wheel.Pending[index] = nil
	end
end

local function slotLabels()
	local labels = {}
	for index, id in ipairs(Wheel.slots) do
		labels[index] = Wheel.gestures[id].label
	end
	return table.concat(labels, "|")
end

-- Calls made before the movie is ready are lost, so the first open sends this from OnReady.
local function sendContent()
	UIAction.CallFunction(ELEMENT, INSTANCE, "SetSlots", slotLabels())
	UIAction.CallFunction(ELEMENT, INSTANCE, "SetCenter", CENTER_LABEL)
	UIAction.CallFunction(ELEMENT, INSTANCE, "Reset")
end

local function isAvailable(entity)
	if entity == player or entity.soul == nil or entity.human == nil or entity.actor == nil then
		return false
	end
	local ok, dead = pcall(function() return entity.actor:IsDead() end)
	return ok and not dead
end

-- The nearest living human in front of the camera, or nil.
local function findTarget()
	local origin = player:GetWorldPos()
	local view = System.GetViewCameraDir()
	local viewLength = math.max(math.sqrt(view.x * view.x + view.y * view.y), 0.01)
	local target, nearest = nil, TARGET_RANGE
	for _, entity in pairs(System.GetEntitiesInSphere(origin, TARGET_RANGE) or {}) do
		if isAvailable(entity) then
			local position = entity:GetWorldPos()
			local dx, dy = position.x - origin.x, position.y - origin.y
			local distance = math.max(math.sqrt(dx * dx + dy * dy), 0.01)
			local dot = (dx * view.x + dy * view.y) / (distance * viewLength)
			if dot >= TARGET_MIN_DOT and distance < nearest then
				target, nearest = entity, distance
			end
		end
	end
	return target
end

-- The next of count lines for this NPC and gesture, in turn: an NPC asked for one bark type again repeats its line.
local function nextTurn(npc, gesture, count)
	local turns = Wheel.barkTurns[npc.id] or {}
	Wheel.barkTurns[npc.id] = turns
	local turn = (turns[gesture.id] or 0) % count + 1
	turns[gesture.id] = turn
	return turn
end

-- The game drops a message whose table misses a member of its type, and answers with the number 0 instead of a boolean.
local function sendBark(npc, metarole)
	local request = {
		metarole = metarole,
		isDramaticBark = false,
		canBeDelayed = false,
		priority = "addressingPlayer",
		lookAtId = player.soul:GetId(),
		contextAsText = "",
		animationApproach = "dontPlayDialogAnimations",
	}
	return XGenAIModule.SendMessageToEntityData(npc.this.id, BARK_MESSAGE, request)
end

local function changeReputation(npc, row)
	local now = System.GetCurrTime()
	local last = Wheel.reputationTimes[npc.id]
	if last ~= nil and now - last < REPUTATION_COOLDOWN_S then
		return false
	end
	Wheel.reputationTimes[npc.id] = now
	npc.soul:ModifyPlayerReputation(row, false)
	return true
end

local function attackHenry(npc)
	local request = { target = player.soul:GetId(), priorityTarget = true }
	return XGenAIModule.SendMessageToEntityData(npc.this.id, ATTACK_MESSAGE, request)
end

-- Counts a provoking gesture against the NPC and tells whether it reached the reaction's fight count, each within its
-- window of the last, which starts the count again.
local function provoked(npc, fight)
	local now = System.GetCurrTime()
	local record = Wheel.threats[npc.id]
	if record == nil or now - record.time > fight.window then
		record = { count = 0 }
		Wheel.threats[npc.id] = record
	end
	record.count = record.count + 1
	record.time = now
	if record.count < fight.count then
		return false
	end
	Wheel.threats[npc.id] = nil
	return true
end

-- Whether the NPC is on the move: its walk shows as MotionMovement, and the speed catches a start or a stop.
local function isMoving(npc, state)
	if state == WALKING_STATE then
		return true
	end
	local velocity = npc.GetVelocity and npc:GetVelocity()
	return velocity ~= nil and velocity.x * velocity.x + velocity.y * velocity.y > MOVING_SPEED * MOVING_SPEED
end

-- The reaction's fragment for what the NPC is doing, named by its current fragment: an idle NPC stands, SittingIdle and
-- Party_Sitting_Talking sit. A walking NPC would slide along the ground in a full-body answer (its AI keeps moving it),
-- and anything else has its hands busy or its body placed (ReadingBook on a bench, EatingMash, Leaning_Back_Loop,
-- SweepingFloor, combat), so it returns nil and the NPC only speaks. A woman plays only what her database has, else the
-- built-in nod or refusal.
local function answerFragment(npc, reaction)
	local state = tostring(npc.actor:GetCurrentAnimationState())
	if isMoving(npc, state) then
		return nil
	end
	local seated = state:find("Motion", 1, true) ~= 1 and state:find("Sitting", 1, true) ~= nil
	if state:find("Motion", 1, true) ~= 1 and not seated then
		return nil
	end
	local list = seated and reaction.seated or reaction.animations
	if #list == 0 then
		return nil
	end
	local woman = npc.soul:GetGender() == FEMALE
	local fitting = {}
	for _, animation in ipairs(list) do
		if animation.women or not woman then
			fitting[#fitting + 1] = animation.fragment
		end
	end
	if #fitting == 0 then
		return reaction.change < 0 and FALLBACK_ANSWERS.refuse or FALLBACK_ANSWERS.agree
	end
	return fitting[math.random(#fitting)]
end

-- The NPC says the reaction's next line: a bark type (its own voice) or an exact line (through Sound Framework).
local function speak(npc, gesture, reaction)
	local count = #reaction.barks + #reaction.lines
	if count == 0 then
		return "silent"
	end
	local turn = nextTurn(npc, gesture, count)
	if turn <= #reaction.barks then
		local metarole = reaction.barks[turn]
		return metarole .. ":" .. tostring(sendBark(npc, metarole))
	end
	local line = reaction.lines[turn - #reaction.barks]
	System.ExecuteCommand(string.format(SAY_COMMAND, npc:GetName(), line))
	return "line " .. line
end

local function answer(npc, gesture)
	if not isAvailable(npc) then
		return
	end
	local reaction = Wheel.reactions[gesture.reaction]
	if reaction == nil then
		return
	end
	if reaction.fight and provoked(npc, reaction.fight) then
		log(gesture.label .. ": " .. npc:GetName() .. " attacks, sent=" .. tostring(attackHenry(npc)))
		return
	end
	local fragment = answerFragment(npc, reaction)
	if fragment then
		npc.human:PlayAnim(fragment, "")
	end
	local said = speak(npc, gesture, reaction)
	local changed = reaction.reputation ~= nil and changeReputation(npc, reaction.reputation)
	log(string.format("%s: %s answers (%s), fragment=%s said=%s reputation=%s", gesture.label, npc:GetName(), reaction.label, tostring(fragment), said, tostring(changed)))
end

-- A random index of the list, never last while the list has another, or nil for an empty list.
local function pickAtRandom(list, last)
	if #list == 0 then
		return nil
	end
	local index = math.random(#list)
	if index == last then
		index = index % #list + 1
	end
	return index
end

-- The prop of the lit torch or lamp in Henry's light hand, or nil. Its light is not on the prop: the game keeps it on
-- him (TORCH_LIGHT) while he holds one lit.
local function heldLight()
	if System.GetEntityByName(TORCH_LIGHT) == nil then
		return nil
	end
	local item = ItemManager.GetItem(player.human:GetItemInHand(LIGHT_HAND))
	return item and item.entity and System.GetEntity(item.entity)
end

-- A gesture made for empty hands hides the torch while it plays and leaves its light on, so the night does not go dark.
-- It hides the whole prop: DrawSlot on a held item's slots crashes the game.
local function hideTorch()
	local prop = heldLight()
	if prop and not prop:IsHidden() then
		prop:Hide(1)
		Wheel.hiddenTorch = prop.id
	end
end

-- Shows the hidden torch again. The game may have replaced the prop meanwhile; the new one shows by itself.
showTorch = function()
	if Wheel.hiddenTorch then
		local prop = System.GetEntity(Wheel.hiddenTorch)
		if prop then
			prop:Hide(0)
		end
		Wheel.hiddenTorch = nil
	end
end

-- Whether Henry holds an item in hand (WEAPON_HAND or LIGHT_HAND).
local function holds(hand)
	return ItemManager.GetItem(player.human:GetItemInHand(hand)) ~= nil
end

-- The weapon set Henry has out (MELEE_SET or RANGED_SET), or nil while none is drawn and in hand.
local function drawnSet()
	if not player.human:IsWeaponDrawn() then
		return nil
	end
	local set
	for hand = WEAPON_HAND, LIGHT_HAND do
		local item = ItemManager.GetItem(player.human:GetItemInHand(hand))
		if item then
			local prop = item.entity and System.GetEntity(item.entity)
			if prop and prop.class == RANGED_CLASS then
				return RANGED_SET
			end
			set = MELEE_SET
		end
	end
	return set
end

-- Takes out a weapon set, then calls drawn. The game switches to the set asked for only with both hands empty: over a
-- drawn set ToggleWeaponSet does nothing, and with a torch in hand it draws the selected set instead. HolsterWeapon is
-- no way to empty them, as it puts away the selected set only, and the player can draw the other one without selecting
-- it (a crossbow out with the melee set selected). So whatever is in hand goes into the bag at once, and keepLight
-- (through drawn) brings a torch back.
local function drawSet(set, drawn)
	local started = System.GetCurrAsyncTime()
	local holstered = {}
	local function check()
		local empty = true
		for hand = WEAPON_HAND, LIGHT_HAND do
			if holds(hand) then
				empty = false
				if not holstered[hand] then
					player.human:HolsterToInventory(hand, false)
					holstered[hand] = true
				end
			end
		end
		if empty and not player.human:IsWeaponDrawn() then
			player.human:ToggleWeaponSet(set)
			if drawn then
				drawn()
			end
		elseif (System.GetCurrAsyncTime() - started) * 1000 < HANDS_WAIT_MS then
			Script.SetTimer(1, check)
		end
	end
	check()
end

-- Drawing or sheathing a two-handed weapon puts the torch or lamp in Henry's light hand into his bag, which puts its
-- light out and leaves him torchless for the guards. This puts it back the frame it goes, without an animation, each
-- time it goes for HANDS_WAIT_MS (a draw can take it again after it came back); the gesture then hides it like any
-- animation without a torch version, light on. After putting it back it waits LIGHT_REDRAW_MS for it to show in hand.
local function keepLight(light)
	local started = System.GetCurrAsyncTime()
	local redrawn
	local function check()
		local now = System.GetCurrAsyncTime()
		if not holds(LIGHT_HAND) and (redrawn == nil or (now - redrawn) * 1000 >= LIGHT_REDRAW_MS) then
			player.human:DrawFromInventory(light, LIGHT_HAND, false)
			redrawn = now
		end
		if (now - started) * 1000 < HANDS_WAIT_MS then
			Script.SetTimer(1, check)
		end
	end
	Script.SetTimer(1, check)
end

-- Takes out or puts away what the gesture's hands need, then calls play once that is done (or after HANDS_WAIT_MS). What
-- it changed goes back when the gesture ends (Wheel.restoreHands); while that is pending from an earlier gesture, the
-- hands stay as they are, so the first change is the one undone. A gesture for one weapon set swaps the other one out
-- for it and back afterwards. A melee weapon keeps the torch in hand (keepLight); a bow needs that hand, so it still
-- puts the torch away.
local function prepareHands(gesture, play)
	local hands = gesture.hands
	local ready
	if hands == nil or Wheel.restoreHands then
		play()
		return
	elseif (hands == "melee" or hands == "ranged") and drawnSet() ~= (hands == "melee" and MELEE_SET or RANGED_SET) then
		local wanted = hands == "melee" and MELEE_SET or RANGED_SET
		local drawn = drawnSet()
		local light = wanted == MELEE_SET and drawn == nil and holds(LIGHT_HAND) and player.human:GetItemInHand(LIGHT_HAND) or nil
		drawSet(wanted, light and function() keepLight(light) end)
		Wheel.restoreHands = function()
			if drawn then
				drawSet(drawn)
			else
				player.human:HolsterWeapon()
			end
			if light then
				keepLight(light)
			end
		end
		ready = function() return drawnSet() == wanted end
	elseif hands == "torch" and not holds(LIGHT_HAND) then
		local torch = player.inventory:FindItem(TORCH_CLASS)
		if torch then
			player.human:DrawFromInventory(torch, LIGHT_HAND, true)
			Wheel.restoreHands = function() player.human:HolsterToInventory(LIGHT_HAND, true) end
			ready = function() return holds(LIGHT_HAND) end
		end
	elseif hands == "empty" and player.human:IsWeaponDrawn() then
		-- The weapons go into the bag at once (HolsterWeapon misses a set drawn without selecting it); a lit torch stays,
		-- for the gesture to hide.
		local set = drawnSet() or MELEE_SET
		local light = heldLight() and player.human:GetItemInHand(LIGHT_HAND)
		for hand = WEAPON_HAND, LIGHT_HAND do
			if holds(hand) and not (light and hand == LIGHT_HAND) then
				player.human:HolsterToInventory(hand, false)
			end
		end
		Wheel.restoreHands = function() drawSet(set, light and function() keepLight(light) end) end
		ready = function() return not player.human:IsWeaponDrawn() end
	end
	if ready == nil then
		play()
		return
	end
	Wheel.handsSerial = (Wheel.handsSerial or 0) + 1
	local serial = Wheel.handsSerial
	local waited, settled = 0, nil
	local function check()
		if serial ~= Wheel.handsSerial then
			return
		end
		waited = waited + HOLD_CHECK_MS
		settled = ready() and (settled or waited) or nil
		if settled and waited - settled >= DRAW_SETTLE_MS or waited >= HANDS_WAIT_MS then
			play()
		else
			Script.SetTimer(HOLD_CHECK_MS, check)
		end
	end
	Script.SetTimer(HOLD_CHECK_MS, check)
end

-- Gives Henry's hands back as they were before the gesture.
local function restoreHands()
	local restore = Wheel.restoreHands
	Wheel.restoreHands = nil
	if restore then
		restore()
	end
end

-- A prop entity of a hand ("right", "left"), left over or current, goes.
local function removeProp(hand)
	local entity = System.GetEntityByName(PROP_NAME .. hand)
	if entity then
		System.RemoveEntity(entity.id)
	end
end

-- Takes the prop entities of a Wheel.props state out of Henry's hands (or off him, in front) and removes them.
local function removeEntities(state)
	for index, id in pairs(state.entities) do
		if index ~= PROP_FRONT then
			player.human:DetachFromHand(index)
		end
		System.RemoveEntity(id)
	end
end

-- Puts the gesture's props in Henry's hands: what a hand holds goes into the bag first, then the model is attached to
-- it; an EMPTY_HAND prop only empties the hand. A lit torch or lamp is hidden instead, so its light stays on. The player
-- sets each hand, so nothing else is put away. A prop in front is attached to Henry himself and opens if the game opens
-- that model when it is read (Wheel.PropOpen). Wheel.props keeps the entities (by hand index, or PROP_FRONT), what went
-- into the bag and the weapon set that was out, for detachProps. A gesture picked while another still plays takes over:
-- the other's props go at once, and what they put in the bag stays there until this one ends, so Henry gets back what
-- he had before the first (as with restoreHands).
local function attachProps(gesture, posture)
	local previous = Wheel.props
	Wheel.props = nil
	if previous then
		removeEntities(previous)
	end
	if gesture.props == nil then
		if previous then
			Wheel.props = { entities = {}, models = {}, items = previous.items, set = previous.set }
		end
		return
	end
	local state = { entities = {}, models = {}, items = previous and previous.items or {}, set = previous and previous.set or drawnSet() }
	for hand, prop in pairs(gesture.props) do
		local index = PROP_HANDS[hand] or PROP_FRONT
		removeProp(hand)
		if index == LIGHT_HAND and heldLight() then
			-- A lit torch or lamp would put its light out in the bag (the game does): it is hidden instead, its light
			-- staying on, and shows again when the gesture ends (showTorch).
			hideTorch()
		elseif index ~= PROP_FRONT and holds(index) then
			state.items[index] = state.items[index] or player.human:GetItemInHand(index)
			player.human:HolsterToInventory(index, false)
		end
		local entity = prop.model ~= Wheel.EMPTY_HAND and System.SpawnEntity({
			class = PROP_CLASS,
			name = PROP_NAME .. hand,
			position = player:GetWorldPos(),
			properties = { object_Model = prop.model, Physics = { bPhysicalize = 0 } },
		})
		if entity then
			local model = string.lower(prop.model)
			if index == PROP_FRONT then
				player:AttachChild(entity.id, 0)
				entity:SetLocalPos(FRONT_PROP_POS[posture] or FRONT_PROP_POS.stand)
				entity:SetLocalAngles(FRONT_PROP_ANGLES)
				local open = Wheel.PropOpen and Wheel.PropOpen[model]
				if open then
					entity:StartAnimation(0, open, 0, 0, 1, false)
				end
			else
				player.human:AttachEntityToHand(entity.id, index)
			end
			state.entities[index] = entity.id
			state.models[index] = model
		elseif prop.model ~= Wheel.EMPTY_HAND then
			log("could not show the prop " .. prop.model)
		end
	end
	Wheel.props = state
end

-- Takes the props out of Henry's hands and gives back what they held: a weapon set that went into the bag comes back as
-- a set (with the torch or lamp it had beside it), anything else into the hand it came from. A set the gesture's hands
-- took out is left to restoreHands, which puts back what Henry had before: drawing it here as well would race with that.
local function detachProps()
	local state = Wheel.props
	Wheel.props = nil
	if state == nil then
		return
	end
	removeEntities(state)
	-- A bow is held in the light hand, a melee weapon in the weapon hand.
	local weaponTaken = state.set == MELEE_SET and state.items[WEAPON_HAND] or state.set == RANGED_SET and state.items[LIGHT_HAND]
	if weaponTaken then
		if Wheel.restoreHands then
			return
		end
		local light = state.set == MELEE_SET and (state.items[LIGHT_HAND] or holds(LIGHT_HAND) and player.human:GetItemInHand(LIGHT_HAND)) or nil
		drawSet(state.set, light and function() keepLight(light) end)
	else
		for index, item in pairs(state.items) do
			player.human:DrawFromInventory(item, index, false)
		end
	end
end

-- Undoes what startRun set for a gesture: Henry's collider, the still and stop maps, the props in his hands, the hidden
-- torch and what his hands held. A gesture that takes over sets them its own way instead.
local function endHold()
	player:SetColliderMode(COLLIDER_RESTORED)
	ActionMapManager.EnableActionMap(STILL_ACTION_MAP, false)
	ActionMapManager.EnableActionMap(STOP_ACTION_MAP, false)
	Wheel.playing = nil
	Wheel.run = nil
	detachProps()
	showTorch()
	restoreHands()
end

local function playTags(partial, optionTags)
	local tags = {}
	if partial then
		tags[#tags + 1] = OWN_SCOPES_TAG
	end
	if optionTags then
		tags[#tags + 1] = optionTags
		tags[#tags + 1] = TEMPORARY_TAGS_TAG
	end
	return table.concat(tags, "+")
end

-- tags asking for option number option of the matched tags as well, when one was chosen.
local function withOption(tags, option)
	if option == nil then
		return tags
	end
	local tag = OPTION_TAG_PREFIX .. option
	return tags ~= "" and tags .. "+" .. tag or tag
end

-- A fragment with no option for Henry's tags shows his bind pose, so a gesture plays on the tags of one of its options,
-- wherever he stands: a chair's fragment sits him on the air. The option is chosen when the fragment starts, so the
-- tags come off then. The face and upper-body fragments never show in the state, and get the whole wait.
local function releaseTags(fragment)
	Wheel.tagSerial = (Wheel.tagSerial or 0) + 1
	local serial = Wheel.tagSerial
	local waited = 0
	local function check()
		if serial ~= Wheel.tagSerial then
			return
		end
		waited = waited + HOLD_CHECK_MS
		if tostring(player.actor:GetCurrentAnimationState()) == fragment or waited >= HOLD_START_MS then
			player.human:PlayAnim(RESTORE_TAGS_FRAGMENT, "")
		else
			Script.SetTimer(HOLD_CHECK_MS, check)
		end
	end
	Script.SetTimer(HOLD_CHECK_MS, check)
end

local function henryPosture()
	if player.human:IsMounted() then
		return "horse"
	end
	return player.player:IsSitting() and "seat" or "stand"
end

-- A tag set as one text, its tags sorted, as Wheel.UpperSets keys them.
local function tagKey(tags)
	local list = {}
	for tag in string.gmatch(tags or "", "[^+]+") do
		list[#list + 1] = tag
	end
	table.sort(list)
	return table.concat(list, "+")
end

-- The upper-body version of a standing animation, which plays on a seat or a horse, or nil. An option the player picked
-- from one of the fragment's tag sets (happy, drunk, a tempo) plays that set's own twin (Wheel.UpperSets) with the same
-- option number; a plain entry, or an option from the fragment's main standing set, plays the main twin.
local function upperFor(animation)
	if animation.partial or animation.set == SEAT_SET or animation.set == HORSE_SET then
		return nil
	end
	local sets = Wheel.UpperSets and Wheel.UpperSets[animation.fragment]
	local own = sets and sets[tagKey(animation.tags)]
	if own then
		return { fragment = animation.fragment, upper = own, option = animation.option }
	end
	local twin = Wheel.UpperBody and Wheel.UpperBody[animation.fragment]
	if twin and (animation.option == nil or animation.set == STAND_SET) then
		return { fragment = animation.fragment, upper = twin, option = animation.option }
	end
	return nil
end

-- What Henry picks from in a posture: the gesture's own list for it, else those of its standing animations that have an
-- option there, else their upper-body versions, else the stock fallback for its reaction, which has options
-- everywhere.
local function animationsFor(gesture, posture)
	local own = posture == "seat" and gesture.seated or posture == "horse" and gesture.mounted or gesture.animations
	if #own > 0 then
		return own
	end
	local fitting = {}
	if posture ~= "stand" then
		for _, animation in ipairs(gesture.animations) do
			if animation[posture] ~= nil then
				fitting[#fitting + 1] = animation
			end
		end
		if #fitting == 0 then
			for _, animation in ipairs(gesture.animations) do
				-- A twin copies its set's options in order, so one of them keeps its number there.
				fitting[#fitting + 1] = upperFor(animation)
			end
		end
	end
	if #fitting > 0 then
		return fitting
	end
	return { FALLBACK_ANIMATIONS[gesture.reaction] or FALLBACK_ANIMATION }
end
-- The editor shows what an empty posture list plays with the same rule.
Wheel.AnimationsFor = animationsFor

-- The tags given, without the empty ones, joined with +.
local function joinTags(...)
	local tags = {}
	for index = 1, select("#", ...) do
		local tag = select(index, ...)
		if tag ~= nil and tag ~= "" then
			tags[#tags + 1] = tag
		end
	end
	return table.concat(tags, "+")
end

-- What one animation of a sequence part plays in a posture: itself standing or where it has an option for it, else its
-- upper-body twin (with the same option, see animationsFor), else nothing.
local function fitFor(animation, posture)
	if posture == "stand" or animation[posture] ~= nil then
		return animation
	end
	return upperFor(animation)
end

-- Plays animation with the extra tags (QUEUE_TAG, HOLD_TAG). An option for the posture plays on its tags, one the player
-- chose for it anyway on its standing ones, one chosen option on its own; held, a fragment whose options end on a
-- blank plays its hold twin. Returns the name Henry's state shows while it plays (nil for a twin on the upper body or a
-- fragment for part of the body, which never show) and whether its tags must come off once it starts.
local function playEntry(animation, posture, ...)
	if animation.upper then
		-- The twin is defined on the UpperBody scope only, so the seat or saddle keeps the full body.
		player.human:PlayAnim(animation.upper, joinTags(withOption(OWN_SCOPES_TAG, animation.option), ...))
		return nil, false
	end
	local held = false
	for index = 1, select("#", ...) do
		held = held or select(index, ...) == HOLD_TAG
	end
	local fragment = held and Wheel.HoldTwins and Wheel.HoldTwins[animation.fragment] or animation.fragment
	local tags = animation.option == nil and posture ~= "stand" and animation[posture] or animation.tags
	player.human:PlayAnim(fragment, joinTags(withOption(playTags(animation.partial, tags ~= "" and tags or nil), animation.option), ...))
	return not animation.partial and fragment or nil, tags ~= nil and tags ~= ""
end

-- One part of a sequence, picked at random from its list (never the same twice in a row) and fitted to the posture.
local function pickPart(gesture, role, posture)
	local list = gesture[role]
	local key = role .. posture
	gesture.last[key] = pickAtRandom(list, gesture.last[key])
	local animation = list[gesture.last[key]]
	return animation and fitFor(animation, posture)
end

-- The gesture playing (Wheel.run): its parts in order (intro and loop, or the one animation), the outro that ends it,
-- the part Henry's state shows, and when its duration is over (stopAt, on the game clock). A part queues behind the one
-- before once that one shows, so it starts where the other ends; the loop and a held last part stay on their last
-- pose. The duration or a movement key plays the outro, or without one stops the gesture.
local function endRun(run)
	if Wheel.run == run then
		Wheel.holdSerial = (Wheel.holdSerial or 0) + 1
		endHold()
	end
end

-- The game plays some fragments on the item in hand as well (a book opens and turns its pages while Henry reads, a bow
-- bends while he draws it): Wheel.PropAnimations, from the model's own animation database. This plays that clip on each
-- prop for the part's fragment: the option of the clip Henry plays, else the one with the part's fragment tags and
-- posture tags. A one-shot clip stays on its last frame.
local function animateProps(run, part)
	local state = Wheel.props
	if state == nil or Wheel.PropAnimations == nil or part.propsAnimated then
		return
	end
	part.propsAnimated = true
	local animation = part.animation
	local clip = animation.clip and string.lower(string.match(animation.clip, "([^/\\]+)$") or ""):gsub("%.caf$", "") or nil
	local postureTags = run.posture ~= "stand" and animation[run.posture] or nil
	for index, id in pairs(state.entities) do
		local fragments = Wheel.PropAnimations[state.models[index]]
		local options = fragments and fragments[animation.fragment]
		local entity = options and System.GetEntity(id)
		if entity then
			local best, bestScore
			for _, option in ipairs(options) do
				local score = (option.clip == clip and 4 or 0) + (option.fragTags == (animation.tags or "") and 2 or 0)
					+ ((postureTags and option.tags == postureTags or postureTags == nil and option.tags == "") and 1 or 0)
				if bestScore == nil or score > bestScore then
					best, bestScore = option, score
				end
			end
			entity:StartAnimation(0, best.clip, 0, PROP_BLEND_S, 1, best.loop)
		end
	end
end

-- How long an animation Henry's state never shows plays (an upper-body twin, a fragment for part of the body), in seconds
-- (Wheel.Lengths, from the game's clip index): its option's, or its longest option's when the game picks one. nil when
-- unknown or when it ends on a loop.
local function playLength(animation)
	local list = Wheel.Lengths and Wheel.Lengths[animation.upper or animation.fragment]
	if list == nil then
		return nil
	end
	local length = animation.option ~= nil and list[animation.option + 1] or nil
	if length == nil then
		for _, value in ipairs(list) do
			length = (length == nil or value < 0 or (length >= 0 and value > length)) and value or length
		end
	end
	return length ~= nil and length >= 0 and length or nil
end

local function playPart(run, part, queued)
	local holds = part.role == "loop" or run.gesture.hold and part.last
	-- An upper-body twin the game would pick a variant of at random: the wheel picks it, so its length is that variant's.
	local animation = part.animation
	local variants = animation.upper and animation.option == nil and Wheel.Lengths and Wheel.Lengths[animation.upper]
	if variants and #variants > 1 then
		part.animation = { fragment = animation.fragment, upper = animation.upper, option = math.random(#variants) - 1 }
	end
	part.state, part.tags = playEntry(part.animation, run.posture, queued and QUEUE_TAG or nil, holds and HOLD_TAG or nil)
	-- Times are on the game clock, which stops while the game is paused, as the animation does. The Lua timers that
	-- check the run fire on frames, later than asked at a low frame rate, so counting them would run slow.
	local now = System.GetCurrTime()
	part.played, part.at = true, now
	-- A part Henry's state never shows ends by its length, counted from the end of the part it queues behind.
	local length = part.state == nil and not holds and playLength(part.animation)
	if length then
		local start = queued and math.max(now, run.freeAt or now) or now
		part.endsAt = start + length + LENGTH_GRACE_S
		run.freeAt = part.endsAt
	end
	-- A queued part starts when the one before ends, which checkRun sees; a part that never shows in Henry's state (the
	-- upper body, part of the body) is taken as started now.
	if not queued or part.state == nil then
		animateProps(run, part)
	end
end

local function finishRun(run)
	local outro = run.outro
	if outro and not outro.played then
		run.stopping = true
		run.index = #run.parts + 1
		run.parts[run.index] = outro
		playPart(run, outro, false)
	else
		player.human:StopAnim()
		endRun(run)
	end
end

-- Every frame of a standing full-body gesture: Henry walking off while it holds his pose slides across the ground, and
-- with his collider off for the gesture (startRun) nothing keeps him on it, so he can end up in the air. The movement
-- keys and the stick end a gesture when pressed, which one already held when it starts never is; so it also ends,
-- collider back, once Henry really moves (SLIDE_SPEED over SLIDE_WINDOW_S) or falls, whatever moves him. His position,
-- not his speed: GetVelocity reads 0 while the stick slides him in a gesture's pose.
local function watchSlide(run)
	if Wheel.run ~= run then
		return
	end
	local now, pos = System.GetCurrTime(), player:GetWorldPos()
	local samples = run.slideSamples
	samples[#samples + 1] = { t = now, x = pos.x, y = pos.y, z = pos.z }
	-- The oldest sample kept is the newest one at least SLIDE_WINDOW_S old.
	while #samples > 2 and now - samples[2].t >= SLIDE_WINDOW_S do
		table.remove(samples, 1)
	end
	local from = samples[1]
	local span = now - from.t
	if span >= SLIDE_WINDOW_S then
		local speed = math.sqrt((pos.x - from.x) ^ 2 + (pos.y - from.y) ^ 2) / span
		local fall = (from.z - pos.z) / span
		if speed > SLIDE_SPEED or fall > FALL_SPEED then
			log(string.format("Henry: moving at %.1f m/s (%.1f down), stopped %s", speed, fall, tostring(Wheel.playing)))
			player.human:StopAnim()
			endRun(run)
			return
		end
	end
	Script.SetTimer(0, function() watchSlide(run) end)
end

-- Henry's state shows the part playing; a part made for the upper body or part of the body never shows, so its run ends
-- by its length (Wheel.Lengths), its duration or a movement key.
local function checkRun(run)
	if run.serial ~= Wheel.holdSerial then
		return
	end
	local now = System.GetCurrTime()
	-- Henry mounted, dismounted, sat down or stood up while it played (only the movement keys stop a gesture): the parts
	-- were picked for the posture he had, and a held one would keep him standing on the horse. StopAnim ends only the
	-- gesture's own action, never the game's mount or seat action.
	local posture = henryPosture()
	if posture ~= run.posture then
		log("Henry: " .. run.posture .. " -> " .. posture .. ", stopped " .. tostring(Wheel.playing))
		player.human:StopAnim()
		endRun(run)
		return
	end
	-- A torch drawn while it plays (or put back in hand by the game) is hidden like the one Henry held at the start.
	if run.hidesTorch then
		hideTorch()
	end
	local state = tostring(player.actor:GetCurrentAnimationState())
	for index = run.index, #run.parts do
		if run.parts[index].state == state then
			run.index = index
			run.parts[index].shown = true
			run.shown = true
			animateProps(run, run.parts[index])
		end
	end
	local current = run.parts[run.index]
	if not current.followed and (current.shown or (now - current.at) * 1000 >= HOLD_START_MS) then
		current.followed = true
		if current.tags then
			player.human:PlayAnim(RESTORE_TAGS_FRAGMENT, "")
		end
		local following = run.parts[run.index + 1]
		if following and not following.played then
			playPart(run, following, true)
		elseif run.outro and not run.outro.played and current.role == "intro" then
			-- Without a loop the outro follows the intro by itself.
			run.stopping = true
			run.parts[#run.parts + 1] = run.outro
			playPart(run, run.outro, true)
		end
	end
	if run.stopAt and now >= run.stopAt and not run.stopping then
		finishRun(run)
		if Wheel.run ~= run then
			return
		end
	end
	-- Over when the last part has played and Henry's state shows none of the parts any more: the last one has shown and
	-- ended, or it never started.
	local onPart = false
	for _, part in ipairs(run.parts) do
		onPart = onPart or part.state == state
	end
	local last = run.parts[#run.parts]
	if last.played and last.endsAt and now >= last.endsAt then
		endRun(run)
		return
	end
	if last.played and last.state ~= nil and not onPart and (last.shown or (now - last.at) * 1000 >= HOLD_START_MS) then
		endRun(run)
		return
	end
	Script.SetTimer(HOLD_CHECK_MS, function() checkRun(run) end)
end

local function startRun(gesture, posture, parts, outro)
	Wheel.holdSerial = (Wheel.holdSerial or 0) + 1
	local run = { serial = Wheel.holdSerial, gesture = gesture, posture = posture, parts = parts, outro = outro, index = 1 }
	local limit = gesture.duration or gesture.lock and HOLD_LIMIT_S or nil
	run.stopAt = limit and System.GetCurrTime() + limit or nil
	parts[#parts].last = outro == nil
	if outro then
		outro.last = true
	end
	-- Only a full-body animation needs Henry's collider off and, locked, the still map. The torch stays in view only when
	-- every part has an option made for it, which Henry's torch tag then picks.
	local fullBody, torchFits = false, true
	for _, part in ipairs(outro and { outro, unpack(parts) } or parts) do
		local animation = part.animation
		if not animation.upper and not animation.partial then
			fullBody = true
		end
		torchFits = torchFits and not animation.upper and Wheel.TorchVersions ~= nil and Wheel.TorchVersions[animation.fragment] == true
	end
	-- Only a full-body gesture standing can slide; a locked one keeps Henry still through the game's own map.
	run.slideGuard = posture == "stand" and fullBody and not gesture.lock
	-- A torch taken out for the gesture stays in view; empty hands hide it whatever the animation. Then the props: a hand
	-- that gets one gives up its weapon, and hides its lit torch (attachProps).
	showTorch()
	local hidesTorch = gesture.hands == "empty" or gesture.hands ~= "torch" and not torchFits
	run.hidesTorch = hidesTorch or (gesture.props ~= nil and gesture.props.left ~= nil)
	if hidesTorch then
		hideTorch()
	end
	attachProps(gesture, posture)
	Wheel.run = run
	Wheel.playing = parts[1].animation.fragment
	-- With passThrough Henry's collider is off: the game ends a fragment of his as soon as his collider touches an NPC (a
	-- hug ends at the first touch), unless the fragment turns it off itself; without it he can pass through NPCs and walls.
	player:SetColliderMode(fullBody and gesture.passThrough and COLLIDER_DISABLED or COLLIDER_RESTORED)
	-- With the collider off nothing slows Henry down: a 0.3 m/s push keeps 0.3 m/s through the whole gesture, and drops
	-- to 0 within 0.3 s with the collider back. A gesture picked while he still coasts from a walk, as a gamepad player
	-- does right after letting the stick go, would carry him along in its pose: he stops here.
	if fullBody and gesture.passThrough then
		local velocity = player:GetVelocity()
		player:SetVelocity({ x = 0, y = 0, z = velocity and velocity.z or 0 })
	end
	ActionMapManager.EnableActionMap(STILL_ACTION_MAP, fullBody and gesture.lock == true)
	ActionMapManager.EnableActionMap(STOP_ACTION_MAP, not gesture.lock)
	playPart(run, parts[1], false)
	Script.SetTimer(HOLD_CHECK_MS, function() checkRun(run) end)
	if run.slideGuard then
		run.slideSamples = {}
		Script.SetTimer(0, function() watchSlide(run) end)
	end
end

-- A movement key, jump or the left stick (source, for the log) while a gesture plays: during the intro or the loop it
-- plays the outro; during the outro it waits for it, unless the outro holds its last pose; anything else stops at once.
function Wheel.Stop(source)
	local run = Wheel.run
	if run == nil then
		return
	end
	if run.outro and not run.outro.played then
		log("Henry: " .. (source or "key") .. ", outro of " .. tostring(Wheel.playing))
		finishRun(run)
	elseif run.outro and not run.gesture.hold then
		return
	else
		log("Henry: " .. (source or "key") .. ", stopped " .. tostring(Wheel.playing))
		player.human:StopAnim()
		endRun(run)
	end
end

-- The left stick moved past its dead zone (STICK_ACTION_MAP): it ends a gesture that moving ends (not a locked one), as
-- a movement key does.
function Wheel.Move()
	local run = Wheel.run
	if run and not run.gesture.lock then
		Wheel.Stop("left stick")
	end
end

-- A gesture with an intro, a loop or an outro plays them as one sequence.
local function isSequence(gesture)
	return #gesture.intro + #gesture.loop + #gesture.outro > 0
end
Wheel.IsSequence = isSequence

local function perform(slot)
	local gesture = Wheel.gestures[Wheel.slots[slot + 1]]
	if gesture == nil then
		return
	end
	local target = gesture.reaction and findTarget()
	local posture = henryPosture()
	local parts, outro = {}, nil
	if isSequence(gesture) then
		for _, role in ipairs({ "intro", "loop" }) do
			local animation = pickPart(gesture, role, posture)
			if animation then
				parts[#parts + 1] = { animation = animation, role = role }
			end
		end
		local animation = pickPart(gesture, "outro", posture)
		outro = animation and { animation = animation, role = "outro" }
		if #parts == 0 and outro then
			parts, outro = { { animation = outro.animation, role = "single" } }, nil
		end
	else
		local animations = animationsFor(gesture, posture)
		gesture.last[posture] = pickAtRandom(animations, gesture.last[posture])
		local animation = animations[gesture.last[posture]]
		if animation then
			parts[1] = { animation = animation, role = "single" }
		end
	end
	local first = parts[1] and parts[1].animation
	if first and first.partial and not first.upper and #parts == 1 and not outro and not gesture.hold and not gesture.duration and not gesture.props then
		-- A face or upper-body fragment plays over what Henry does, and nothing holds him for it.
		local _, tags = playEntry(first, posture)
		if tags then
			releaseTags(first.fragment)
		end
	elseif first then
		prepareHands(gesture, function() startRun(gesture, posture, parts, outro) end)
	end
	gesture.lastVoice = pickAtRandom(gesture.voice, gesture.lastVoice)
	local voice = gesture.voice[gesture.lastVoice]
	if voice then
		System.ExecuteCommand(SPEAK_COMMAND .. voice)
	end
	local names = {}
	for _, part in ipairs(parts) do
		local animation = part.animation
		names[#names + 1] = animation.fragment .. (animation.option and " option " .. animation.option or "") .. (animation.upper and ", upper body" or "")
	end
	if outro then
		names[#names + 1] = "then " .. outro.animation.fragment
	end
	log(string.format("Henry: %s (%s: %s), target %s", gesture.label, posture, #names > 0 and table.concat(names, " > ") or "nil",
		tostring(target and target:GetName())))
	if target then
		Script.SetTimer(ANSWER_DELAY_MS, function() answer(target, gesture) end)
	end
end

function Wheel.Open()
	if Wheel.isOpen or Wheel.Editor.isOpen or not player or not player.soul then
		return
	end
	registerPending()
	Wheel.isOpen = true
	ActionMapManager.EnableActionMap(OPEN_ACTION_MAP, true)
	UIAction.ShowElement(ELEMENT, INSTANCE)
	if Wheel.movieReady then
		sendContent()
	end
	CryAction.PauseGame(true)
end

function Wheel.Close()
	if not Wheel.isOpen then
		return
	end
	Wheel.isOpen = false
	-- RB's release went to the open wheel's map, not to the one that tracks it.
	Wheel.pad.held = false
	ActionMapManager.EnableActionMap(OPEN_ACTION_MAP, false)
	UIAction.HideElement(ELEMENT, INSTANCE)
	CryAction.PauseGame(false)
end

function Wheel.Release()
	if not Wheel.isOpen then
		return
	end
	local slot = Wheel.hovered
	Wheel.Close()
	perform(slot)
end

-- Gamepad (KCD2_GestureWheel_Profile.xml): hold RB and press Y to open, the right stick picks a slot, releasing RB acts,
-- B cancels. The stick's directions arrive as on/off commands (PAD_ACTION_MAP, always on, so the state is the stick's
-- even while the wheel is shut); two on at once are a diagonal. Slot 0 is at the top and the others go clockwise, as
-- the movie draws them. Letting the stick go keeps the slot, and a stick already pushed when the wheel opens points at
-- nothing until it moves.
function Wheel.PadHold(down)
	Wheel.pad.held = down
end

function Wheel.PadOpen()
	if Wheel.pad.held then
		Wheel.Open()
	end
end

local function padPoint(state)
	local vertical = state.up and "up" or state.down and "down"
	local horizontal = state.right and "right" or state.left and "left"
	local slot = PAD_SLOTS[vertical and horizontal and vertical .. "+" .. horizontal or vertical or horizontal or ""]
	if slot and slot ~= Wheel.hovered then
		Wheel.hovered = slot
		UIAction.CallFunction(ELEMENT, INSTANCE, "SetHover", tostring(slot))
	end
end

local OPPOSITE = { up = "down", down = "up", left = "right", right = "left" }

function Wheel.PadStick(direction, on)
	local state = Wheel.pad.stick
	-- A stick swept across the middle presses the far side before it releases the near one, and it never points both
	-- ways.
	if on then
		state[OPPOSITE[direction]] = nil
	end
	state[direction] = on or nil
	if not Wheel.isOpen then
		return
	end
	Wheel.pad.serial = (Wheel.pad.serial or 0) + 1
	if on then
		padPoint(state)
		return
	end
	-- A stick let go from a diagonal crosses one axis a moment before the other, which would point at the neighbour
	-- just before the release: an off waits PAD_SETTLE_MS, then the slot follows only a stick still pushed (turned
	-- from a diagonal to a side). The movie times the wait (OnSettle): no Lua timer runs while the game is paused, as
	-- it is while the wheel is open.
	UIAction.CallFunction(ELEMENT, INSTANCE, "Settle", tostring(Wheel.pad.serial), tostring(PAD_SETTLE_MS))
end

function Wheel:OnSettle(elementName, instanceId, eventName, args)
	local serial = type(args) == "table" and (args[0] or args[1] or args.Serial) or args
	if Wheel.isOpen and tonumber(serial) == Wheel.pad.serial then
		padPoint(Wheel.pad.stick)
	end
end

-- Opens the editor on the hovered slot.
function Wheel.Edit()
	if not Wheel.isOpen or Wheel.hovered == NO_SLOT then
		return
	end
	local slot = Wheel.hovered + 1
	Wheel.Close()
	Wheel.Editor.Open(slot)
end

function Wheel:OnReady(elementName, instanceId, eventName, args)
	Wheel.movieReady = true
	sendContent()
end

function Wheel:OnHover(elementName, instanceId, eventName, args)
	Wheel.hovered = slotFrom(args)
end

function Wheel:OnPick(elementName, instanceId, eventName, args)
	Wheel.hovered = slotFrom(args)
	Wheel.Release()
end

function Wheel:OnCancel(elementName, instanceId, eventName, args)
	Wheel.Close()
end

function Wheel:OnGameplayStarted(actionName, eventName, args)
	-- A prop a gesture had in hand when the game saved or the scripts reloaded is not Henry's: it goes.
	for _, hand in ipairs(PROP_PLACES) do
		removeProp(hand)
	end
	Wheel.props = nil
	registerPending()
	Wheel.LoadSettings()
	if Wheel.listening then
		return
	end
	Wheel.listening = true
	for _, event in ipairs({ "OnReady", "OnHover", "OnPick", "OnCancel", "OnSettle" }) do
		UIAction.RegisterElementListener(Wheel, ELEMENT, -1, event, event)
	end
	Wheel.Editor.Listen()
end

for index, id in ipairs(DEFAULT_SLOTS) do
	Wheel.slots[index] = id
end
for _, definition in ipairs(DEFAULT_REACTIONS) do
	Wheel.RegisterReaction(definition)
end
for _, definition in ipairs(DEFAULT_GESTURES) do
	Wheel.Register(definition)
end

System.AddCCommand("gesturewheel_open", "GestureWheel.Open()", "Opens the gesture wheel")
System.AddCCommand("gesturewheel_release", "GestureWheel.Release()", "Acts on the wheel's hovered slot and closes it")
System.AddCCommand("gesturewheel_edit", "GestureWheel.Edit()", "Opens the editor on the wheel's hovered slot")
System.AddCCommand("gesturewheel_cancel", "GestureWheel.Close()", "Closes the gesture wheel without acting")
System.AddCCommand("gesturewheel_pad_hold_down", "GestureWheel.PadHold(true)", "Gamepad: RB pressed")
System.AddCCommand("gesturewheel_pad_hold_up", "GestureWheel.PadHold(false)", "Gamepad: RB released")
System.AddCCommand("gesturewheel_pad_open", "GestureWheel.PadOpen()", "Gamepad: Y, which opens the wheel while RB is held")
for _, direction in ipairs({ "up", "down", "left", "right" }) do
	for _, on in ipairs({ true, false }) do
		System.AddCCommand(string.format("gesturewheel_pad_r_%s_%s", direction, on and "on" or "off"),
			string.format("GestureWheel.PadStick(%q, %s)", direction, tostring(on)), "Gamepad: a right stick direction")
	end
end
for _, command in ipairs(STOP_COMMANDS) do
	System.AddCCommand(command, string.format("GestureWheel.Stop(%q)", command), "Ends the gesture playing")
end
for _, axis in ipairs(STICK_AXES) do
	System.AddCCommand("gesturewheel_move_" .. axis, "GestureWheel.Move()", "Gamepad: the left stick moved")
end
ActionMapManager.LoadFromXML(PROFILE)
ActionMapManager.EnableActionMap(ACTION_MAP, true)
ActionMapManager.EnableActionMap(STICK_ACTION_MAP, true)
ActionMapManager.EnableActionMap(PAD_ACTION_MAP, true)
UIAction.RegisterEventSystemListener(Wheel, "System", "OnGameplayStarted", "OnGameplayStarted")
Script.ReloadScript("Scripts/GestureWheel/UpperBody.lua")
Script.ReloadScript("Scripts/GestureWheel/PropAnimations.lua")
Script.ReloadScript("Scripts/GestureWheel/Settings.lua")
Script.ReloadScript("Scripts/GestureWheel/Editor.lua")
-- Rerun in a loaded world (hot reload): OnGameplayStarted has already fired, and the defaults above replaced the
-- player's slots and custom gestures.
if player and Wheel.LoadSettings then
	Wheel.LoadSettings()
end
log("loaded")
