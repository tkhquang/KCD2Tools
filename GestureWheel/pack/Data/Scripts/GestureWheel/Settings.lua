-- Gesture Wheel settings: the player's choices from the editor, in Mods/gesture_wheel/gesture_wheel.settings through
-- the Animation Framework.
local Wheel = GestureWheel

local MOD_ID = "gesture_wheel"
-- The framework's store: text goes over this console variable, the commands read and write the mod's file.
local STORE_TEXT = "animfw_settings_text"
local STORE_LOAD = "animfw_settings_load " .. MOD_ID
local STORE_SAVE = "animfw_settings_save " .. MOD_ID
-- A line of the file is a key, a space and its value. Keys it does not know are skipped.
local SLOTS_KEY = "slots"
local SLOT_SEPARATOR = "|"
-- A custom gesture's line: its id as the key, then its fields joined with FIELD_SEPARATOR: name, standing animations,
-- voices, reaction, seat animations, horse animations, collision, intro, loop and outro animations, hold "1",
-- duration in seconds, hands, locked "1", and
-- the props of the right hand, the left hand and in front, each its model and its name joined with PART_SEPARATOR. A
-- missing trailing field reads as empty. A list field joins its entries with LIST_SEPARATOR, and an animation's entry
-- is its fragment, tags, partial flag, its options for a seat and a horse ("seat:<tags>", "horse:<tags>", empty for
-- none) and, for one chosen option, OPTION_PREFIX and its number, set and clip, joined with PART_SEPARATOR. Collision
-- holds KEEP_COLLISION for a gesture that keeps Henry's collision while it plays, and nothing for one that passes
-- through.
local FIELD_SEPARATOR = "\t"
local LIST_SEPARATOR = ";"
local PART_SEPARATOR = ","
local KEEP_COLLISION = "0"
-- An NPC reaction the player changed or built: its key is this and its id, its fields the name, the standing and seated
-- animations ("fragment,women" entries, women for one a woman can play), the bark types, the exact lines, the
-- reputation row, its change, and the fight rule ("count/window", empty for none).
local REACTION_KEY = "reaction_"
local WOMEN_MARK = "women"
local FIGHT_SEPARATOR = "/"
local SEAT_PREFIX = "seat:"
local HORSE_PREFIX = "horse:"
-- An animation entry for one chosen option ends with "option:<number>:<set>:<clip>".
local OPTION_PREFIX = "option:"
local OPTION_SEPARATOR = ":"

local function log(text)
	System.LogAlways("[GestureWheel] " .. text)
end

local function animationsText(animations)
	local entries = {}
	for index, animation in ipairs(animations) do
		local seat = animation.seat and SEAT_PREFIX .. animation.seat or ""
		local horse = animation.horse and HORSE_PREFIX .. animation.horse or ""
		local option = animation.option and table.concat({ OPTION_PREFIX .. animation.option, animation.set or "", animation.clip or "" }, OPTION_SEPARATOR) or nil
		entries[index] = table.concat({ animation.fragment, animation.tags or "", animation.partial and "1" or "", seat, horse, option }, PART_SEPARATOR)
	end
	return table.concat(entries, LIST_SEPARATOR)
end

local function npcAnimationsText(animations)
	local entries = {}
	for index, animation in ipairs(animations) do
		entries[index] = animation.fragment .. PART_SEPARATOR .. (animation.women and WOMEN_MARK or "")
	end
	return table.concat(entries, LIST_SEPARATOR)
end

-- A hand's prop as a field: its model and its name (separators in the name become spaces).
local function propText(prop)
	if prop == nil then
		return ""
	end
	local label = string.gsub(prop.label or "", "[" .. FIELD_SEPARATOR .. LIST_SEPARATOR .. PART_SEPARATOR .. "\n]", " ")
	return prop.model .. PART_SEPARATOR .. label
end

local function propFrom(text)
	local model, label = string.match(text or "", "^([^,]+),?(.*)$")
	return model and { model = model, label = label ~= "" and label or model } or nil
end

local function reactionText(reaction)
	local fight = reaction.fight and reaction.fight.count .. FIGHT_SEPARATOR .. reaction.fight.window or ""
	return table.concat({ reaction.label, npcAnimationsText(reaction.animations), npcAnimationsText(reaction.seated), table.concat(reaction.barks, LIST_SEPARATOR), table.concat(reaction.lines, LIST_SEPARATOR), reaction.reputation or "", tostring(reaction.change), fight }, FIELD_SEPARATOR)
end

function Wheel.SaveSettings()
	local lines = {
		SLOTS_KEY .. " " .. table.concat(Wheel.slots, SLOT_SEPARATOR),
	}
	for _, id in ipairs(Wheel.reactionOrder) do
		local reaction = Wheel.reactions[id]
		if reaction.edited or string.find(id, Wheel.CUSTOM_REACTION_PREFIX, 1, true) == 1 then
			lines[#lines + 1] = REACTION_KEY .. id .. " " .. reactionText(reaction)
		end
	end
	for _, id in ipairs(Wheel.gestureOrder) do
		local gesture = Wheel.gestures[id]
		if string.find(id, Wheel.CUSTOM_PREFIX, 1, true) == 1 then
			local fields = {
				gesture.label,
				animationsText(gesture.animations),
				table.concat(gesture.voice, LIST_SEPARATOR),
				gesture.reaction or "",
				animationsText(gesture.seated),
				animationsText(gesture.mounted),
				gesture.passThrough and "" or KEEP_COLLISION,
				animationsText(gesture.intro),
				animationsText(gesture.loop),
				animationsText(gesture.outro),
				gesture.hold and "1" or "",
				gesture.duration and tostring(gesture.duration) or "",
				gesture.hands or "",
				gesture.lock and "1" or "",
				propText(gesture.props and gesture.props.right),
				propText(gesture.props and gesture.props.left),
				propText(gesture.props and gesture.props.front),
			}
			lines[#lines + 1] = id .. " " .. table.concat(fields, FIELD_SEPARATOR)
		end
	end
	System.SetCVar(STORE_TEXT, table.concat(lines, "\n"))
	System.ExecuteCommand(STORE_SAVE)
end

-- The tags after prefix in an entry's part, or nil when the part is not that option.
local function optionTags(part, prefix)
	return string.find(part, prefix, 1, true) == 1 and string.sub(part, #prefix + 1) or nil
end

local function animationsFrom(text)
	local animations = {}
	for entry in string.gmatch(text or "", "[^" .. LIST_SEPARATOR .. "]+") do
		local fragment, tags, partial, seat, horse, chosen = string.match(entry, "^([^,]*),?([^,]*),?([^,]*),?([^,]*),?([^,]*),?([^,]*)$")
		local option, set, clip = string.match(chosen or "", "^" .. OPTION_PREFIX .. "(%d+):([^:]*):(.*)$")
		animations[#animations + 1] = {
			fragment = fragment,
			tags = tags ~= "" and tags or nil,
			partial = partial == "1" or nil,
			seat = optionTags(seat, SEAT_PREFIX),
			horse = optionTags(horse, HORSE_PREFIX),
			option = tonumber(option),
			set = set ~= "" and set or nil,
			clip = clip ~= "" and clip or nil,
		}
	end
	return animations
end

local function registerCustom(id, line)
	local fields = {}
	for field in string.gmatch(line .. FIELD_SEPARATOR, "([^\t]*)\t") do
		fields[#fields + 1] = field
	end
	local label, animations, voices, reaction, seated, mounted, collision, intro, loop, outro, hold, duration, hands, lock, rightProp, leftProp, frontProp = unpack(fields)
	if #fields < 4 then
		log("ignored a damaged saved gesture: " .. id)
		return
	end
	local voice = {}
	for file in string.gmatch(voices, "[^" .. LIST_SEPARATOR .. "]+") do
		voice[#voice + 1] = file
	end
	Wheel.Register({
		id = id,
		label = label,
		fragment = animationsFrom(animations),
		seated = animationsFrom(seated),
		mounted = animationsFrom(mounted),
		intro = animationsFrom(intro),
		loop = animationsFrom(loop),
		outro = animationsFrom(outro),
		hold = hold == "1" or nil,
		duration = tonumber(duration),
		hands = hands ~= "" and hands or nil,
		lock = lock == "1" or nil,
		props = { right = propFrom(rightProp), left = propFrom(leftProp), front = propFrom(frontProp) },
		voice = voice,
		reaction = reaction ~= "" and reaction or nil,
		passThrough = collision ~= KEEP_COLLISION,
	})
end

local function listFrom(text)
	local list = {}
	for entry in string.gmatch(text or "", "[^" .. LIST_SEPARATOR .. "]+") do
		list[#list + 1] = entry
	end
	return list
end

local function npcAnimationsFrom(text)
	local animations = {}
	for _, entry in ipairs(listFrom(text)) do
		local fragment, women = string.match(entry, "^([^,]*),?([^,]*)$")
		animations[#animations + 1] = { fragment = fragment, women = women == WOMEN_MARK }
	end
	return animations
end

local function registerReaction(id, line)
	local label, animations, seated, barks, voices, reputation, change, fight = string.match(line, "^([^\t]*)\t([^\t]*)\t([^\t]*)\t([^\t]*)\t([^\t]*)\t([^\t]*)\t([^\t]*)\t([^\t]*)$")
	if label == nil then
		log("ignored a damaged saved reaction: " .. id)
		return
	end
	local count, window = string.match(fight, "^(%d+)" .. FIGHT_SEPARATOR .. "(%d+)$")
	Wheel.RegisterReaction({
		id = id,
		label = label,
		animations = npcAnimationsFrom(animations),
		seated = npcAnimationsFrom(seated),
		barks = listFrom(barks),
		lines = listFrom(voices),
		reputation = reputation,
		change = tonumber(change),
		fight = count and { count = tonumber(count), window = tonumber(window) } or nil,
		edited = true,
	})
end

-- The keys of saved that start with prefix, a shorter one (a lower number) first.
local function keysWith(saved, prefix)
	local keys = {}
	for key in pairs(saved) do
		if string.find(key, prefix, 1, true) == 1 then
			keys[#keys + 1] = key
		end
	end
	table.sort(keys, function(a, b) return #a < #b or #a == #b and a < b end)
	return keys
end

-- Puts the saved choices back. A gesture whose mod is gone leaves its slot as it is.
function Wheel.LoadSettings()
	System.ExecuteCommand(STORE_LOAD)
	-- Without the framework's store (not running, or its console commands not up yet) the wheel keeps its built-in
	-- gestures rather than stopping its start-up.
	local text = System.GetCVar(STORE_TEXT)
	if text == nil then
		log("settings: the Animation Framework's store is missing, built-in gestures only")
		return
	end
	local saved = {}
	for key, value in string.gmatch(tostring(text), "(%S+) ([^\n]*)") do
		saved[key] = value
	end
	-- Reactions first: the gestures name them. Both in the order they were made (custom_2 before custom_10), which the
	-- editor lists them in.
	for _, key in ipairs(keysWith(saved, REACTION_KEY)) do
		registerReaction(string.sub(key, #REACTION_KEY + 1), saved[key])
	end
	for _, key in ipairs(keysWith(saved, Wheel.CUSTOM_PREFIX)) do
		registerCustom(key, saved[key])
	end
	local slot = 0
	for id in string.gmatch(saved[SLOTS_KEY] or "", "[^" .. SLOT_SEPARATOR .. "]+") do
		slot = slot + 1
		if Wheel.gestures[id] ~= nil and Wheel.slots[slot] ~= nil then
			Wheel.slots[slot] = id
		end
	end
end
