-- Gesture Wheel editor: the picker that builds a custom gesture for a slot. Search an animation and a voice line, choose
-- how NPCs react and name it. The Animation Framework and Sound Framework build the lists of animations and voice lines
-- from the game's files and search them, so these follow game updates and mods; the wheel asks for one page at a time.
-- The bark types and reputation changes are in Catalog.lua, loaded when the editor opens and dropped when it closes.
local Wheel = GestureWheel
local Editor = Wheel.Editor or {}
Wheel.Editor = Editor

local ELEMENT = "KCD2_GestureEditor"
local INSTANCE = 0
local ACTION_MAP = "gesture_wheel_editor"
local CATALOG = "Scripts/GestureWheel/Catalog.lua"
-- A search goes over a console variable: the text in, a command, the answer back on the next line of the script. An
-- answer's first line is how many lines match, or a word for why there are none yet.
local ANIMATION_TEXT = "animfw_catalog_text"
local ANIMATION_FIND = "animfw_catalog_find %s %d %d %s"
local ANIMATION_LINE = "animfw_catalog_line "
local ANIMATION_HIDE = "animfw_catalog_hide "
-- The wheel's own fragments, the upper-body and hold twins it plays in the place of a stock one.
local INTERNAL_FRAGMENTS = { "GWU_", "GWH_" }
local VOICE_TEXT = "sfw_lines_text"
local VOICE_FIND = "sfw_lines_find %d %d"
local VOICE_LINE = "sfw_lines_line "
local BUILDING_ANSWER = "building"
local BUILDING_LABEL = "Sound Framework is listing the game's voice lines (a few seconds, the first time only)..."
local NO_LIST_LABEL = "Nothing to list: see the game's log"
local NO_VOICES_LABEL = "Browsing voice lines needs Sound Framework"
-- How often a list that is being built is asked again, in milliseconds.
local LIST_CHECK_MS = 500
local SPEAK_COMMAND = "wh_snd_playfile Localization/dialog/"
local TAB_ANIMATION = 0
local TAB_SEQUENCE = 1
local TAB_VOICE = 2
local TAB_REACTION = 3
local TAB_NAME = 4
local TAB_LABELS = "Animation|Sequence|Voice line|NPC reaction|Name & options"
-- The sequence tab's sections: the parts, each a list of animations, and the timing rows.
local PARTS = {
	{ list = "intro", label = "In", help = "Plays first, once. Click an animation to add or remove it; picking an In fills the matching Loop and Out when the game has them." },
	{ list = "loop", label = "Loop", help = "Plays after the In until the gesture ends (the timing), and holds a one-shot on its last pose. Click to add or remove." },
	{ list = "outro", label = "Out", help = "Plays when the gesture ends or Henry moves, once, back to his idle. Click to add or remove." },
}
local TIMING = "Timing"
local PART_TITLE = "Part:"
local TIMING_HELP = "Hold keeps the last animation on its last pose. The time counts from the start and ends the loop or the hold; moving always ends it."
local HOLD_LABEL = "Hold the last pose"
local HOLD_NOTE = "instead of going back to his idle"
local DURATION_UNTIL_MOVED = "Ends when Henry moves"
local DURATION_LABEL = "Ends after %d s"
local DURATIONS = { 3, 5, 10, 20, 30, 60 }
-- The game names a three-part action's fragments these ways: the In's ending, then its Loop's and Out's.
local PART_ENDINGS = {
	{ "_In", "_Loop", "_Out" },
	{ "In", "Loop", "Out" },
	{ "_Start", "_Loop", "_End" },
	{ "Start", "Loop", "End" },
}
local PAGE_ROWS = 12
local SCROLL_ROWS = 3
local NEW_LINE = 10
local NO_REACTION = "None"
-- The animation tab edits one list at a time, what Henry plays standing, on a seat or on a horse, picked by the posture
-- buttons. For a seat or a horse the filter box limits the search to the animations with an option there.
local POSTURES = {
	{ id = "stand", list = "animations", label = "Standing", where = "Standing" },
	{ id = "seat", list = "seated", label = "On a seat", word = "seat", where = "On a seat" },
	{ id = "horse", list = "mounted", label = "On horseback", word = "horse", where = "On horseback" },
}
local FILTER_LABEL = "[%s] Has a %s version"
local POSTURE_TITLE = "Plays when:"
local CLEAR_ANIMATIONS = "Clear list"
local CLEAR_VOICES = "Silent"
-- How the catalog writes a fragment's options for a seat and a horse.
local SEAT_PREFIX = "seat:"
local HORSE_PREFIX = "horse:"
-- An option line's first character in the catalog, and the sets an option can be in (Catalog: clips).
local CLIP_MARK = "@"
local SET_STAND = "stand"
local SET_SEAT = "seat"
local SET_HORSE = "horse"
-- How a result line tells the movie the gesture has it (gold) or not, and where its note starts.
local CHOSEN_MARK = "*"
local PLAIN_MARK = " "
local NOTE_SEPARATOR = "\t"
-- What each tab's help line says (the animation tab's says what Henry plays).
local VOICE_HELP = "Henry says one of the gold lines at random. Click a line to hear it and add or remove it."
local NAME_HELP = "Type the name the wheel shows. Click an option to turn it on (gold) or off, pick what Henry does with his hands, or put another gesture on this slot as it is. Right hand and Left hand put any prop in a hand."
-- The rows of the last tab: a gesture's on/off options, each a field of the draft, and the note that explains it.
local OPTIONS = {
	{ field = "passThrough", label = "No collision", note = "touching NPCs no longer stops it; can pass through walls", default = true },
	{ field = "lock", label = "Locked in", note = "moving does not end it, menus stay shut; a loop stops after 15 s", default = false },
}
-- An option the draft leaves unset is at its default.
local OPTION_DEFAULTS = {}
for _, option in ipairs(OPTIONS) do
	OPTION_DEFAULTS[option.field] = option.default
end
-- The rows under the hands that put a registered gesture (built-in, another mod's or one of the player's) on the slot.
local USE_LABEL = "Use: %s"
local USE_NOTE = "put this gesture on the slot as it is"
local USE_OWN_NOTE = "put your gesture from another slot on this one"
-- The Hands choice under the options: what the gesture does with what Henry holds, undone when it ends.
local HANDS = {
	{ label = "Hands: as they are", note = "a torch hides where the animation has no torch version" },
	{ hands = "melee", label = "Hands: melee weapon out", note = "draws his sword, mace, axe... for it" },
	{ hands = "ranged", label = "Hands: ranged weapon out", note = "draws his bow or crossbow for it" },
	{ hands = "torch", label = "Hands: torch out", note = "takes a torch from his bag for it" },
	{ hands = "empty", label = "Hands: empty", note = "puts the weapon away, hides the torch (its light stays)" },
}
local WEAPON_NOTE = "versions for %s"
-- The last tab's sections: the options, then what each hand holds while the gesture plays, a prop (any model of the
-- game's items: a fishing rod, a lantern, a mug, a sword). The search box types the name on Options and searches the
-- props on a hand.
local NAME_SECTIONS = {
	{ label = "Options" },
	{ label = "Right hand", hand = "right" },
	{ label = "Left hand", hand = "left" },
	{ label = "In front", hand = "front", front = true },
}
local NAME_TITLE = "Edit:"
local PROP_HELP = "%s: Henry holds the gold prop while the gesture plays, or nothing with Empty, and gets back what he held after. Type to search; a click picks or drops one."
local PROP_ANIMATION_NOTE = "comes with %s"
local FRONT_HELP = "In front: the gold prop stays in front of Henry, where the game holds a book he reads (the holding_book_idle pose holds it there); a book opens. Type to search; a click picks or drops one."
local EMPTY_PROP_LABEL = "Empty"
local EMPTY_PROP_NOTE = "what this hand holds goes into the bag while the gesture plays"
local CLEAR_PROP = "As it is"
-- The reaction tab's sections: which reaction the gesture uses, then the parts of that reaction, which every gesture
-- using it shares.
local SECTION_REACTION, SECTION_ANIMATION, SECTION_SEATED, SECTION_BARKS, SECTION_LINES, SECTION_REPUTATION, SECTION_FIGHT = 1, 2, 3, 4, 5, 6, 7
local SECTION_LABELS = { "Reaction", "Animation", "Seated", "Barks", "Lines", "Reputation", "Fight" }
local SECTION_TITLE = "NPC:"
local NEW_REACTION = "New"
local NEW_REACTION_LABEL = "Custom %d"
local MEN_ONLY = "men only"
-- When an NPC attacks: after this many of the gestures using the reaction, each within window seconds of the last.
local FIGHTS = {
	{ label = "Never" },
	{ label = "At once", count = 1, window = 30 },
	{ label = "After 2 within 30 s", count = 2, window = 30 },
	{ label = "After 3 within 30 s", count = 3, window = 30 },
	{ label = "After 5 within 60 s", count = 5, window = 60 },
}
local REACTION_HELP = "How the NPC Henry faces answers. New copies the gold reaction; type a name to rename it."
-- Save with no animation in any list: the wheel takes no gesture without one.
local SAVE_NEEDS_ANIMATION = "Pick an animation first: a gesture needs at least one to save."
local NO_REACTION_HELP = "Choose a reaction in the Reaction section first, or press New to make one."
-- What each part section's help line says after the reaction's name.
local SECTION_HELP = {
	[SECTION_ANIMATION] = "what the NPC plays standing. A woman skips \"men only\" ones (else a nod or refusal).",
	[SECTION_SEATED] = "what a seated NPC plays. A standing animation would lift the NPC out of the seat.",
	[SECTION_BARKS] = "the NPC says one of its own lines for a gold bark type, in turn; some NPCs have none.",
	[SECTION_LINES] = "the NPC says a gold line in its actor's voice (needs Sound Framework). A click plays it.",
	[SECTION_REPUTATION] = "how Henry's standing with the NPC changes, once a minute at most.",
	[SECTION_FIGHT] = "when the NPC attacks Henry.",
}

local function log(text)
	System.LogAlways("[GestureWheel] " .. text)
end

-- Event arguments arrive in a 0-indexed table.
local function argument(args)
	if type(args) == "table" then
		return args[0] or args[1]
	end
	return args
end

local function call(name, value)
	if value == nil then
		UIAction.CallFunction(ELEMENT, INSTANCE, name)
	else
		UIAction.CallFunction(ELEMENT, INSTANCE, name, value)
	end
end

-- The line of the blob around a position, where the next line starts, and where this one does.
local function lineAround(blob, position)
	local first = position
	while first > 1 and string.byte(blob, first - 1) ~= NEW_LINE do
		first = first - 1
	end
	local last = string.find(blob, "\n", position, true) or #blob + 1
	return string.sub(blob, first, last - 1), last + 1, first
end

-- A framework's answer: the page's lines and how many lines match, or nil and the word for why there are none.
local function pageOf(answer)
	if answer == nil then
		return nil, ""
	end
	local newline = string.find(answer, "\n", 1, true)
	local head = newline and string.sub(answer, 1, newline - 1) or answer
	local total = tonumber(head)
	if total == nil then
		return nil, head
	end
	local lines = {}
	for line in string.gmatch(string.sub(answer, newline + 1), "[^\n]+") do
		lines[#lines + 1] = line
	end
	return lines, total
end

-- A tab's results: its own rows (the clear row, the gesture's entries), then the matches of a search, fetched a page at
-- a time as they are shown, so a quarter of a million lines cost a page. search is { find, row, missing }: find(first,
-- count) gives a page from match first (from 0) and how many match, or nil and a word; row(line) makes a line's row;
-- missing is what shows when the list cannot be had.
local function newList(rows, search)
	return { rows = rows, search = search, lines = {}, total = search == nil and 0 or nil }
end

-- Fetches the page that starts at match (from 1). Without one to give the list shows a single row saying why.
local function fetch(list, match)
	local lines, total = list.search.find(match - 1, PAGE_ROWS)
	if lines == nil then
		list.total, list.building = 0, total == BUILDING_ANSWER
		list.notice = list.building and BUILDING_LABEL or list.search.missing or NO_LIST_LABEL
		return
	end
	list.total, list.notice, list.building = total, nil, nil
	for index, line in ipairs(lines) do
		list.lines[match + index - 1] = line
	end
end

-- The rows the list has.
local function listCount(list)
	if list.search == nil then
		return #list.rows
	end
	if list.total == nil then
		fetch(list, 1)
	end
	return #list.rows + list.total + (list.notice and 1 or 0)
end

local function listRow(list, index)
	if index <= #list.rows then
		return list.rows[index]
	end
	if list.search == nil then
		return nil
	end
	local match = index - #list.rows
	if list.total == nil then
		fetch(list, 1)
	end
	if list.notice then
		return match == 1 and { label = list.notice, notice = true } or nil
	end
	if list.lines[match] == nil and match <= list.total then
		fetch(list, match)
	end
	local line = list.lines[match]
	return line and list.search.row(line) or nil
end

-- 260040 as 260,040.
local function grouped(number)
	local text = string.reverse((string.gsub(string.reverse(tostring(math.floor(number))), "(%d%d%d)", "%1,")))
	return (string.gsub(text, "^,", ""))
end

-- A voice line is its file (under Localization/dialog/, without the extension), who says it ("Role (Actor)", maybe empty)
-- and its text. The search looks at who says it and the text, the label shows both.
local function voiceParts(line)
	return string.match(line, "^([^\t]+)\t([^\t]*)\t(.*)$")
end

-- A role can be a whole sentence ("Npc Vyprovazi Hrace Kdyz Ho Videlo V Trespassu..."), so the label keeps its start and
-- the actor; the search still reads all of it.
local MAX_ROLE_CHARS = 24

local function voiceLabel(speaker, text)
	if speaker == "" then
		return text
	end
	local role, actor = string.match(speaker, "^(.-) %(([^()]*)%)$")
	role = role or speaker
	if #role > MAX_ROLE_CHARS then
		role = string.sub(role, 1, MAX_ROLE_CHARS - 1) .. "…"
	end
	return (actor and role .. " (" .. actor .. ")" or role) .. ": " .. text
end

local function voiceRow(line)
	local file, speaker, text = voiceParts(line)
	return file and { label = voiceLabel(speaker, text), value = file } or nil
end

-- The voice lines that hold wanted, from Sound Framework.
local function voiceSearch(wanted)
	return {
		find = function(first, count)
			if System.GetCVar(VOICE_TEXT) == nil then
				return nil, ""
			end
			System.SetCVar(VOICE_TEXT, wanted)
			System.ExecuteCommand(string.format(VOICE_FIND, first, count))
			return pageOf(System.GetCVar(VOICE_TEXT))
		end,
		row = voiceRow,
		missing = NO_VOICES_LABEL,
	}
end

-- A voice file's label from its line, or the file itself while Sound Framework has no line for it.
local function voiceText(file)
	if System.GetCVar(VOICE_TEXT) == nil then
		return file
	end
	System.ExecuteCommand(VOICE_LINE .. file)
	local _, speaker, text = voiceParts(System.GetCVar(VOICE_TEXT) or "")
	return text and voiceLabel(speaker, text) or file
end

-- seat and horse are the tags of the fragment's options there ("" for none needed), nil when it has none. The note says
-- where it has a version of its own, what state it was made for (Henry is put in it for the play) and whether it only
-- moves the face or upper body.
local function fragmentRowOf(name, tags, partial, seat, horse)
	local notes = {}
	if seat and horse then
		notes[1] = "seat, horse"
	elseif seat or horse then
		notes[1] = seat and "seat" or "horse"
	end
	if tags then
		notes[#notes + 1] = "made for " .. tags
	end
	if partial then
		notes[#notes + 1] = "face or upper body"
	end
	local hands = Wheel.HandVersions and Wheel.HandVersions[name]
	if hands and hands.weapons ~= "" then
		notes[#notes + 1] = string.format(WEAPON_NOTE, hands.weapons)
	end
	local note = #notes > 0 and table.concat(notes, "; ") or nil
	return { label = name, note = note, key = name, value = name, partial = partial, tags = tags, seat = seat, horse = horse }
end

-- What tells an animation entry or row from another: its fragment, and for one option of it, the option's tags and
-- number among the options with those tags.
local function animationKey(entry)
	return entry.option and string.format("%s#%d/%s", entry.fragment or entry.value, entry.option, entry.tags or "") or entry.fragment or entry.value
end

-- One option of a fragment, which plays only its clip: option is its number among the options with its tags, set where
-- it belongs (SET_STAND for the fragment's standing ones, SET_SEAT, SET_HORSE, else nil). A seat or horse option is that
-- posture's version of itself.
local function clipRowOf(fragment, clip, tags, option, set, partial)
	local notes = { "variant " .. (option + 1) }
	if set == SET_SEAT or set == SET_HORSE then
		notes[#notes + 1] = set
	end
	if tags then
		notes[#notes + 1] = "made for " .. tags
	end
	if partial then
		notes[#notes + 1] = "face or upper body"
	end
	local row = { label = fragment .. ": " .. clip, note = table.concat(notes, "; "), value = fragment, clip = clip, tags = tags, option = option, set = set, partial = partial }
	row.seat = set == SET_SEAT and (tags or "") or nil
	row.horse = set == SET_HORSE and (tags or "") or nil
	row.key = animationKey(row)
	return row
end

-- An option's line: CLIP_MARK and its fragment, its first clip, its tags, its number, its set, "partial", its other clips.
local function clipRow(line)
	local fragment, clip, tags, option, set, partial = string.match(line, "^" .. CLIP_MARK .. "([^\t]+)\t([^\t]*)\t([^\t]*)\t(%d+)\t([^\t]*)\t([^\t]*)")
	if fragment == nil then
		return nil
	end
	return clipRowOf(fragment, clip, tags ~= "" and tags or nil, tonumber(option), set ~= "" and set or nil, partial ~= "" or nil)
end

-- The tags after prefix in part, or nil when part is not that option.
local function optionTags(part, prefix)
	return part and string.find(part, prefix, 1, true) == 1 and string.sub(part, #prefix + 1) or nil
end

-- A fragment's line is its name, the tags it stands on, whether it covers part of the body, and its options for a seat
-- and a horse.
local function fragmentRow(line)
	local name, tags, partial, seat, horse = string.match(line, "^([^\t]+)\t([^\t]*)\t([^\t]*)\t?([^\t]*)\t?([^\t]*)\t?[^\t]*$")
	return fragmentRowOf(name, tags ~= "" and tags or nil, partial ~= "" or nil, optionTags(seat, SEAT_PREFIX), optionTags(horse, HORSE_PREFIX))
end

local function posture()
	for _, entry in ipairs(POSTURES) do
		if entry.id == Editor.posture then
			return entry
		end
	end
	return POSTURES[1]
end

-- The sequence part edited, nil on the Timing section.
local function part()
	return PARTS[Editor.part]
end

-- The draft's animation list the tab edits: the posture's on the animation tab, the part's on the sequence tab.
local function postureList()
	if Editor.tab == TAB_SEQUENCE then
		return part() and Editor.draft[part().list] or {}
	end
	return Editor.draft[posture().list]
end

-- A reaction with lists of its own, which the editor changes and Save hands to the wheel.
local function copyReaction(reaction)
	local function copied(list)
		local result = {}
		for index, entry in ipairs(list or {}) do
			result[index] = type(entry) == "table" and { fragment = entry.fragment, women = entry.women } or entry
		end
		return result
	end
	return {
		id = reaction.id,
		label = reaction.label,
		animations = copied(reaction.animations),
		seated = copied(reaction.seated),
		barks = copied(reaction.barks),
		lines = copied(reaction.lines),
		reputation = reaction.reputation,
		change = reaction.change or 0,
		fight = reaction.fight and { count = reaction.fight.count, window = reaction.fight.window } or nil,
		edited = reaction.edited,
	}
end

-- The reaction the gesture uses, which the part sections edit, or nil for none.
local function editedReaction()
	local draft = Editor.draft
	return draft.reaction and Editor.reactions[draft.reaction] or nil
end

local function isCustomReaction(id)
	return string.find(id, Wheel.CUSTOM_REACTION_PREFIX, 1, true) == 1
end

-- A bark type's line in the catalog: its metarole, two sample lines and how many lines it has.
local function barkRow(line)
	local metarole, samples, count = string.match(line, "^([^\t]+)\t([^\t]*)\t(%d+)$")
	return { label = samples ~= "" and samples or metarole, value = metarole, note = metarole .. ", " .. count .. " lines" }
end

-- A reputation change's line: its name, the change and whom it reaches.
local function reputationRow(line)
	local name, change, target = string.match(line, "^([^\t]+)\t([^\t]*)\t(.*)$")
	local value = tonumber(change) or 0
	return { label = name, value = name, change = value, note = string.format("%s%s, %s", value > 0 and "+" or "", change, target) }
end

-- The Name & options section shown, and the hand it edits (nil on Options).
local function nameSection()
	return NAME_SECTIONS[Editor.nameSection]
end

-- A prop's row for the hand edited.
local function propRowOf(label, model, note)
	return { label = label, value = model, note = note, prop = model, hand = nameSection().hand }
end

-- A prop's line in the catalog: its name, its model and what kind of item it is.
local function propRow(line)
	local label, model, kind = string.match(line, "^([^\t]+)\t([^\t]+)\t?(.*)$")
	return propRowOf(label or line, model or line, kind ~= "" and kind or nil)
end

-- The props fragments come with (Catalog.fragmentProps), read once per opening: fragment -> hand -> { model, label }.
local function fragmentProps(fragment)
	if Editor.fragmentProps == nil then
		Editor.fragmentProps = {}
		local blob = Wheel.Catalog and Wheel.Catalog.fragmentProps and Wheel.Catalog.fragmentProps[1] or ""
		for line in string.gmatch(blob, "[^\n]+") do
			local name, hand, model, label = string.match(line, "^([^\t]+)\t([^\t]+)\t([^\t]+)\t(.*)$")
			if name then
				Editor.fragmentProps[name] = Editor.fragmentProps[name] or {}
				Editor.fragmentProps[name][hand] = { model = model, label = label }
			end
		end
	end
	return Editor.fragmentProps[fragment]
end

-- A fragment for an NPC: the women's database may lack it.
local function npcRowOf(name, women)
	return { label = name, value = name, women = women, note = not women and MEN_ONLY or nil }
end

local function npcFragmentRow(line)
	local name, women = string.match(line, "^([^\t]+)\t[^\t]*\t[^\t]*\t?[^\t]*\t?[^\t]*\t?([^\t]*)$")
	return npcRowOf(name, women == "women")
end

-- The catalog's row for a bark type, or its bare name when the catalog has none.
local function barkRowOf(metarole)
	local blob = Wheel.Catalog.barks and Wheel.Catalog.barks[1] or ""
	local start = string.sub(blob, 1, #metarole + 1) == metarole .. "\t" and 1 or string.find(blob, "\n" .. metarole .. "\t", 1, true)
	if start then
		return barkRow((lineAround(blob, start == 1 and 1 or start + 1)))
	end
	return { label = metarole, value = metarole, note = metarole }
end

-- One line about what a reaction does, for its row.
local function reactionSummary(reaction)
	local parts = {}
	local count = #reaction.animations
	parts[1] = count == 1 and reaction.animations[1].fragment or count .. " animations"
	parts[#parts + 1] = (#reaction.barks + #reaction.lines) .. " lines"
	if reaction.reputation then
		parts[#parts + 1] = string.format("%s%s", reaction.change > 0 and "+" or "", reaction.change)
	end
	if reaction.fight then
		parts[#parts + 1] = "fights after " .. reaction.fight.count
	end
	return table.concat(parts, ", ")
end

-- The index of the entry of list that test(entry) picks, or nil.
local function indexOf(list, test)
	for index, entry in ipairs(list) do
		if test(entry) then
			return index
		end
	end
	return nil
end

-- Whether an option of the draft is on: as set, else its default.
local function optionOn(draft, field)
	local value = draft[field]
	if value == nil then
		return OPTION_DEFAULTS[field]
	end
	return value
end

local function chosen(result)
	local draft = Editor.draft
	if result.timing == "hold" then
		return draft.hold == true
	elseif result.timing == "duration" then
		return draft.duration == result.seconds
	elseif Editor.tab == TAB_ANIMATION or Editor.tab == TAB_SEQUENCE then
		return indexOf(postureList(), function(animation) return animationKey(animation) == result.key end) ~= nil
	elseif Editor.tab == TAB_VOICE then
		return indexOf(draft.voice, function(file) return file == result.value end) ~= nil
	elseif result.prop then
		local prop = draft.props[result.hand]
		return prop ~= nil and prop.model == result.prop
	elseif result.option then
		return optionOn(draft, result.option)
	elseif result.hands ~= nil then
		return (draft.hands or false) == result.hands
	elseif result.use or result.action or Editor.tab ~= TAB_REACTION then
		return false
	elseif Editor.section == SECTION_REACTION then
		return result.choice == "none" and draft.reaction == nil or result.choice == "reaction" and draft.reaction == result.value
	end
	local reaction = editedReaction()
	if reaction == nil then
		return false
	elseif Editor.section == SECTION_ANIMATION or Editor.section == SECTION_SEATED then
		local list = Editor.section == SECTION_ANIMATION and reaction.animations or reaction.seated
		return indexOf(list, function(animation) return animation.fragment == result.value end) ~= nil
	elseif Editor.section == SECTION_BARKS then
		return indexOf(reaction.barks, function(metarole) return metarole == result.value end) ~= nil
	elseif Editor.section == SECTION_LINES then
		return indexOf(reaction.lines, function(file) return file == result.value end) ~= nil
	elseif Editor.section == SECTION_REPUTATION then
		return result.none and reaction.reputation == nil or reaction.reputation == result.value
	end
	local fight = reaction.fight
	return result.count == nil and fight == nil or fight ~= nil and result.count == fight.count and result.window == fight.window
end

local function clampOffset(offset)
	return math.max(0, math.min(offset, listCount(Editor.list) - PAGE_ROWS))
end

-- The page at Editor.offset, the scrollbar, and the title with which rows of how many show.
local function showRows()
	local list = Editor.list
	-- A search that ran out of matches may hold fewer rows than the offset was made for.
	Editor.offset = clampOffset(Editor.offset)
	local shown = {}
	for row = 1, PAGE_ROWS do
		local result = listRow(list, row + Editor.offset)
		if result == nil then
			break
		end
		local note = result.note and NOTE_SEPARATOR .. result.note or ""
		shown[row] = (chosen(result) and CHOSEN_MARK or PLAIN_MARK) .. result.label .. note
	end
	local total = listCount(list)
	call("SetResults", table.concat(shown, "\n"))
	call("SetScroll", Editor.offset .. "|" .. total)
	local title = "Slot " .. Editor.draft.slot
	if list.search ~= nil and total > 0 and not list.notice then
		title = string.format("%s     %s-%s of %s", title, grouped(Editor.offset + 1), grouped(Editor.offset + #shown), grouped(total))
	end
	call("SetTitle", title)
	if list.building and not list.watched then
		-- The editor pauses the game, so this timer has to run through the pause.
		list.watched = true
		Script.SetTimer(LIST_CHECK_MS, function()
			list.watched = false
			if Editor.isOpen and Editor.list == list then
				list.total, list.notice, list.building = nil, nil, nil
				showRows()
			end
		end, {}, true)
	end
end

local function reactionLabel(draft)
	local reaction = draft.reaction and Editor.reactions[draft.reaction]
	return reaction and reaction.label or NO_REACTION
end

local function showSummary()
	local draft = Editor.draft
	local standing, seated, mounted = #draft.animations, #draft.seated, #draft.mounted
	local animation = standing > 1 and standing .. " animations" or standing == 1 and draft.animations[1].fragment or "none standing"
	if standing + seated + mounted == 0 then
		animation = "no animation"
	end
	animation = seated > 0 and animation .. ", " .. seated .. " on a seat" or animation
	animation = mounted > 0 and animation .. ", " .. mounted .. " on horseback" or animation
	local parts = {}
	for _, entry in ipairs(PARTS) do
		local list = draft[entry.list]
		if #list > 0 then
			parts[#parts + 1] = entry.label .. " " .. (#list == 1 and list[1].fragment or #list .. " animations")
		end
	end
	if #parts > 0 then
		animation = table.concat(parts, " > ")
	end
	if draft.hold then
		animation = animation .. ", holds"
	end
	if draft.duration then
		animation = animation .. string.format(", %d s", draft.duration)
	end
	local held = {}
	for _, entry in ipairs(NAME_SECTIONS) do
		local prop = entry.hand and draft.props[entry.hand]
		if prop then
			held[#held + 1] = prop.model == Wheel.EMPTY_HAND and string.lower(entry.label) .. " empty" or entry.front and prop.label .. " in front" or prop.label
		end
	end
	if #held > 0 then
		animation = animation .. ", with " .. table.concat(held, " and ")
	end
	local voices = #draft.voice
	local voice = voices > 1 and voices .. " voice lines" or voices == 1 and (draft.voiceLabels[draft.voice[1]] or "1 voice line") or "silent"
	call("SetSummary", string.format("%s: %s, %s, %s", draft.label or "No name", animation, voice, reactionLabel(draft)))
end

-- A list tab's own rows: the gesture's entries the search matches (all of them with nothing searched), first, so one
-- click takes any of them out, even one the catalog does not have, such as another mod's. The catalog's matches follow,
-- its entries again among them.
local function ownRows(own, text)
	local wanted = string.lower(text)
	local rows = {}
	for _, row in ipairs(own) do
		if string.find(string.lower(row.label .. "\t" .. row.value .. "\t" .. (row.note or "")), wanted, 1, true) then
			rows[#rows + 1] = row
		end
	end
	return rows
end

local function ownAnimationRows()
	local rows = {}
	for index, animation in ipairs(postureList()) do
		if animation.option then
			rows[index] = clipRowOf(animation.fragment, animation.clip or "?", animation.tags, animation.option, animation.set, animation.partial)
		else
			rows[index] = fragmentRowOf(animation.fragment, animation.tags, animation.partial, animation.seat, animation.horse)
		end
	end
	return rows
end

-- Up to two names of what plays, joined, and how many more there are.
local function playsNames(plays)
	local names = {}
	for index = 1, math.min(#plays, 2) do
		names[index] = plays[index].fragment
	end
	local more = #plays > 2 and string.format(" (or %d more)", #plays - 2) or ""
	return table.concat(names, " or ") .. more
end

-- The animation tab's help line: what Henry plays in the posture being edited, as the wheel picks it (its own list,
-- else the standing animations with a version there, else the built-in gesture).
local function animationHelp()
	local draft = Editor.draft
	local entry = posture()
	local list = draft[entry.list]
	if #list > 0 then
		local what = #list == 1 and list[1].fragment or "one of these " .. #list .. " at random"
		return string.format("%s, Henry plays %s. Click an animation to add or remove it.", entry.where, what)
	end
	local plays = Wheel.AnimationsFor and Wheel.AnimationsFor(draft, entry.id) or {}
	if #plays == 0 then
		return string.format("%s, Henry plays no animation. Pick some to add one.", entry.where)
	end
	local fromStanding = entry.id ~= "stand" and indexOf(draft.animations, function(animation) return animation == plays[1] end)
	if plays[1].upper then
		return string.format("%s, Henry plays %s on his upper body only: your standing animations, over the %s. Pick some to use others.", entry.where, playsNames(plays), entry.id == "horse" and "saddle" or "seat")
	end
	if fromStanding then
		return string.format("%s, Henry plays %s: your standing animations with a %s version. Pick some to use others.", entry.where, playsNames(plays), entry.word)
	end
	return string.format("%s, Henry plays the built-in %s, as nothing chosen fits. Pick some to use others.", entry.where, playsNames(plays))
end

-- The gestures that use a reaction, the slot's own as the draft has it: up to three names and how many more.
local function usedBy(id)
	local names = {}
	local current = Wheel.slots[Editor.draft.slot]
	if Editor.draft.reaction == id then
		names[1] = Editor.draft.label or "this gesture"
	end
	for _, gestureId in ipairs(Wheel.gestureOrder) do
		local gesture = Wheel.gestures[gestureId]
		if gestureId ~= current and gesture.reaction == id then
			names[#names + 1] = gesture.label
		end
	end
	if #names == 0 then
		return "no gesture"
	end
	local shown = { unpack(names, 1, math.min(#names, 3)) }
	return table.concat(shown, ", ") .. (#names > 3 and string.format(" and %d more", #names - 3) or "")
end

-- The reaction tab's buttons, filter box (Delete or Reset on the Reaction section), Clear button (New there) and help.
local function showReactionControls()
	call("SetPostures", SECTION_TITLE .. "|" .. table.concat(SECTION_LABELS, "|") .. "|" .. (Editor.section - 1))
	local reaction = editedReaction()
	if Editor.section == SECTION_REACTION then
		local manage = reaction and (isCustomReaction(reaction.id) and "Delete " .. reaction.label or reaction.edited and "Reset " .. reaction.label) or ""
		call("SetFilter", manage)
		call("SetClear", NEW_REACTION)
		call("SetHelp", REACTION_HELP)
		return
	end
	call("SetFilter", "")
	local list = Editor.section >= SECTION_ANIMATION and Editor.section <= SECTION_LINES
	call("SetClear", reaction and list and CLEAR_ANIMATIONS or "")
	call("SetHelp", reaction and string.format("%s (used by %s): %s", reaction.label, usedBy(reaction.id), SECTION_HELP[Editor.section]) or NO_REACTION_HELP)
end

-- The section buttons, the filter box, the Clear button and the help line for the tab shown.
local function showControls()
	if Editor.tab == TAB_ANIMATION then
		local labels = {}
		local active = 0
		for index, entry in ipairs(POSTURES) do
			labels[index] = string.format("%s (%d)", entry.label, #Editor.draft[entry.list])
			active = entry.id == Editor.posture and index - 1 or active
		end
		call("SetPostures", POSTURE_TITLE .. "|" .. table.concat(labels, "|") .. "|" .. active)
		local word = posture().word
		call("SetFilter", word and string.format(FILTER_LABEL, Editor.filter and "x" or " ", word) or "")
		call("SetClear", CLEAR_ANIMATIONS)
		call("SetHelp", animationHelp())
		return
	elseif Editor.tab == TAB_SEQUENCE then
		local labels = {}
		for index, entry in ipairs(PARTS) do
			labels[index] = string.format("%s (%d)", entry.label, #Editor.draft[entry.list])
		end
		labels[#labels + 1] = TIMING
		call("SetPostures", PART_TITLE .. "|" .. table.concat(labels, "|") .. "|" .. (Editor.part - 1))
		call("SetFilter", "")
		call("SetClear", part() and CLEAR_ANIMATIONS or "")
		call("SetHelp", part() and part().help or TIMING_HELP)
		return
	elseif Editor.tab == TAB_REACTION then
		showReactionControls()
		return
	elseif Editor.tab == TAB_NAME then
		local labels = {}
		for index, entry in ipairs(NAME_SECTIONS) do
			labels[index] = entry.label
		end
		call("SetPostures", NAME_TITLE .. "|" .. table.concat(labels, "|") .. "|" .. (Editor.nameSection - 1))
		call("SetFilter", "")
		local hand = nameSection().hand
		call("SetClear", hand and CLEAR_PROP or "")
		call("SetHelp", nameSection().front and FRONT_HELP or hand and string.format(PROP_HELP, nameSection().label) or NAME_HELP)
		return
	end
	call("SetPostures", "")
	call("SetFilter", "")
	call("SetClear", Editor.tab == TAB_VOICE and CLEAR_VOICES or "")
	call("SetHelp", VOICE_HELP)
end

-- The fragments Henry can play that hold wanted, from the Animation Framework: with options, each option of them too,
-- after them; with only, just what has a version in that posture.
local function animationSearch(wanted, options, only, row)
	return {
		find = function(first, count)
			System.SetCVar(ANIMATION_TEXT, wanted)
			System.ExecuteCommand(string.format(ANIMATION_FIND, options and "all" or "fragments", first, count, only or ""))
			return pageOf(System.GetCVar(ANIMATION_TEXT))
		end,
		row = row,
	}
end

-- The lines of a list in Catalog.lua that hold wanted.
local function localSearch(blob, wanted, row)
	local lines = {}
	for line in string.gmatch(blob or "", "[^\n]+") do
		if wanted == "" or string.find(string.lower(line), wanted, 1, true) then
			lines[#lines + 1] = line
		end
	end
	return {
		find = function(first, count)
			return { unpack(lines, first + 1, math.min(first + count, #lines)) }, #lines
		end,
		row = row,
	}
end

-- The fragments, and with something typed each option of them as well (one clip to play), after them.
local function animationList(text, wanted)
	local rows = ownRows(ownAnimationRows(), text)
	local function row(line)
		if string.sub(line, 1, #CLIP_MARK) == CLIP_MARK then
			return clipRow(line)
		end
		return fragmentRow(line)
	end
	local only = Editor.tab == TAB_ANIMATION and Editor.filter and posture().word and posture().id
	return newList(rows, animationSearch(wanted, wanted ~= "", only, row))
end

local function ownVoiceRows()
	local rows = {}
	for index, file in ipairs(Editor.draft.voice) do
		rows[index] = { label = Editor.draft.voiceLabels[file] or voiceText(file), value = file }
	end
	return rows
end

-- The Reaction section's rows: none, each reaction with what it does, and with a name typed, renaming the gold reaction
-- to it.
local function reactionChoiceRows(text)
	local wanted = string.lower(text)
	local function matches(label)
		return string.find(string.lower(label), wanted, 1, true) ~= nil
	end
	local rows = {}
	if matches(NO_REACTION) then
		rows[1] = { label = NO_REACTION, choice = "none" }
	end
	for _, id in ipairs(Editor.reactionOrder) do
		local reaction = Editor.reactions[id]
		local summary = reactionSummary(reaction)
		if matches(reaction.label .. "\t" .. summary) then
			rows[#rows + 1] = { label = reaction.label, value = id, note = summary, choice = "reaction" }
		end
	end
	local reaction = editedReaction()
	if reaction and text ~= "" and text ~= reaction.label then
		rows[#rows + 1] = { label = string.format('Rename %s to "%s"', reaction.label, text), action = "rename" }
	end
	return rows
end

-- A part section's rows: the reaction's own entries first, then the catalog's (animations, bark types, voice lines,
-- reputation changes), or the fight rules.
local function reactionList(text, wanted)
	if Editor.section == SECTION_REACTION then
		return newList(reactionChoiceRows(text), nil)
	end
	local reaction = editedReaction()
	local catalog = Wheel.Catalog
	if reaction == nil then
		return newList({}, nil)
	elseif Editor.section == SECTION_ANIMATION or Editor.section == SECTION_SEATED then
		local own = {}
		for index, animation in ipairs(Editor.section == SECTION_ANIMATION and reaction.animations or reaction.seated) do
			own[index] = npcRowOf(animation.fragment, animation.women)
		end
		return newList(ownRows(own, text), animationSearch(wanted, false, nil, npcFragmentRow))
	elseif Editor.section == SECTION_BARKS then
		local own = {}
		for index, metarole in ipairs(reaction.barks) do
			own[index] = barkRowOf(metarole)
		end
		return newList(ownRows(own, text), localSearch(catalog.barks and catalog.barks[1], wanted, barkRow))
	elseif Editor.section == SECTION_LINES then
		local own = {}
		for index, file in ipairs(reaction.lines) do
			own[index] = { label = Editor.draft.voiceLabels[file] or voiceText(file), value = file }
		end
		return newList(ownRows(own, text), voiceSearch(wanted))
	elseif Editor.section == SECTION_REPUTATION then
		local rows = string.find(string.lower(NO_REACTION), wanted, 1, true) and { { label = NO_REACTION, none = true } } or {}
		return newList(rows, localSearch(catalog.reputations and catalog.reputations[1], wanted, reputationRow))
	end
	local rows = {}
	for _, fight in ipairs(FIGHTS) do
		rows[#rows + 1] = { label = fight.label, count = fight.count, window = fight.window }
	end
	return newList(rows, nil)
end

-- The fragments of every list of the draft, each once.
local function draftFragments()
	local names, seen = {}, {}
	for _, field in ipairs({ "animations", "seated", "mounted", "intro", "loop", "outro" }) do
		for _, animation in ipairs(Editor.draft[field]) do
			if not seen[animation.fragment] then
				seen[animation.fragment] = true
				names[#names + 1] = animation.fragment
			end
		end
	end
	return names
end

-- A hand section's rows: Empty, the hand's prop, the props the gesture's animations come with in that hand (the game's
-- NPCs hold them), then the catalog's props.
local function propList(text, wanted)
	local hand = nameSection().hand
	-- Empty is for a hand; in front there is nothing to empty.
	local own = nameSection().front and {} or { propRowOf(EMPTY_PROP_LABEL, Wheel.EMPTY_HAND, EMPTY_PROP_NOTE) }
	local seen = { [Wheel.EMPTY_HAND] = true }
	local prop = Editor.draft.props[hand]
	if prop and not seen[prop.model] then
		own[#own + 1] = propRowOf(prop.label, prop.model)
		seen[prop.model] = true
	end
	for _, fragment in ipairs(draftFragments()) do
		local suggested = fragmentProps(fragment)
		suggested = suggested and suggested[hand]
		if suggested and not seen[suggested.model] then
			seen[suggested.model] = true
			own[#own + 1] = propRowOf(suggested.label, suggested.model, string.format(PROP_ANIMATION_NOTE, fragment))
		end
	end
	local props = Wheel.Catalog.props
	return newList(ownRows(own, text), localSearch(props and props[1], wanted, propRow))
end

local function runSearch(text)
	Editor.offset = 0
	Editor.text = text
	local wanted = string.lower(text)
	if Editor.tab == TAB_ANIMATION or Editor.tab == TAB_SEQUENCE and part() then
		Editor.list = animationList(text, wanted)
	elseif Editor.tab == TAB_SEQUENCE then
		local rows = { { label = HOLD_LABEL, note = HOLD_NOTE, timing = "hold" }, { label = DURATION_UNTIL_MOVED, timing = "duration" } }
		for _, seconds in ipairs(DURATIONS) do
			rows[#rows + 1] = { label = string.format(DURATION_LABEL, seconds), timing = "duration", seconds = seconds }
		end
		Editor.list = newList(rows, nil)
	elseif Editor.tab == TAB_VOICE then
		Editor.list = newList(ownRows(ownVoiceRows(), text), voiceSearch(wanted))
	elseif Editor.tab == TAB_REACTION then
		Editor.list = reactionList(text, wanted)
	elseif nameSection().hand then
		-- On a hand the box searches the props; the name stays.
		Editor.list = propList(text, wanted)
	else
		local rows = {}
		for index, option in ipairs(OPTIONS) do
			rows[index] = { label = option.label, note = option.note, value = option.field, option = option.field }
		end
		for _, entry in ipairs(HANDS) do
			rows[#rows + 1] = { label = entry.label, note = entry.note, hands = entry.hands or false }
		end
		for _, id in ipairs(Wheel.gestureOrder) do
			if id ~= Wheel.slots[Editor.draft.slot] then
				local own = string.find(id, Wheel.CUSTOM_PREFIX, 1, true) == 1
				rows[#rows + 1] = { label = string.format(USE_LABEL, Wheel.gestures[id].label), note = own and USE_OWN_NOTE or USE_NOTE, use = id }
			end
		end
		Editor.list = newList(rows, nil)
		Editor.draft.label = text ~= "" and text or nil
		showSummary()
	end
	showControls()
	showRows()
end

-- The search box's text when a tab or a Name & options section opens: the name on Options, else empty.
local function openedText(tab)
	return tab == TAB_NAME and not nameSection().hand and (Editor.draft.label or "") or ""
end

local function selectTab(tab)
	Editor.tab = tab
	call("SetTab", tab)
	local text = openedText(tab)
	call("SetField", text)
	runSearch(text)
end

function Editor.Open(slot)
	if Editor.isOpen or not player or not player.soul or not Wheel.slots[slot] then
		return
	end
	if System.GetCVar(ANIMATION_TEXT) == nil then
		log("the Animation Framework has no animation list")
		return
	end
	-- ReloadScript does nothing for a file it has run, and the catalog was dropped when the editor last closed.
	Script.LoadScript(CATALOG, true)
	if Wheel.Catalog == nil then
		log("the catalog is missing")
		return
	end
	Editor.isOpen = true
	for _, prefix in ipairs(INTERNAL_FRAGMENTS) do
		System.ExecuteCommand(ANIMATION_HIDE .. prefix)
	end
	-- Starts Sound Framework's list now, so it is there by the time the Voice tab opens.
	voiceSearch("").find(0, 1)
	local current = Wheel.gestures[Wheel.slots[slot]]
	-- The lists are copies: the draft changes them, and Cancel must leave the gesture as it was.
	Editor.draft = {
		slot = slot,
		label = current.label,
		animations = { unpack(current.animations) },
		seated = { unpack(current.seated) },
		mounted = { unpack(current.mounted) },
		intro = { unpack(current.intro or {}) },
		loop = { unpack(current.loop or {}) },
		outro = { unpack(current.outro or {}) },
		hold = current.hold,
		hands = current.hands,
		duration = current.duration,
		voice = { unpack(current.voice) },
		voiceLabels = {},
		reaction = current.reaction,
		passThrough = current.passThrough,
		lock = current.lock,
		props = { right = current.props and current.props.right, left = current.props and current.props.left, front = current.props and current.props.front },
	}
	Editor.posture = POSTURES[1].id
	Editor.part = 1
	Editor.nameSection = 1
	-- The catalog was loaded again: so are the props its fragments come with. A hand the player set keeps its choice
	-- when an animation picked later comes with a prop.
	Editor.fragmentProps = nil
	Editor.propsSet = {}
	Editor.filter = true
	-- The reactions are copies too, handed back on Save: edits reach every gesture that uses one.
	Editor.reactions = {}
	Editor.reactionOrder = {}
	Editor.removedReactions = {}
	for index, id in ipairs(Wheel.reactionOrder) do
		Editor.reactions[id] = copyReaction(Wheel.reactions[id])
		Editor.reactionOrder[index] = id
	end
	Editor.section = SECTION_REACTION
	ActionMapManager.EnableActionMap(ACTION_MAP, true)
	CryAction.PauseGame(true)
	UIAction.ShowElement(ELEMENT, INSTANCE)
	call("SetTabs", TAB_LABELS)
	showSummary()
	selectTab(TAB_ANIMATION)
end

function Editor.Close()
	if not Editor.isOpen then
		return
	end
	Editor.isOpen = false
	ActionMapManager.EnableActionMap(ACTION_MAP, false)
	UIAction.HideElement(ELEMENT, INSTANCE)
	CryAction.PauseGame(false)
	Wheel.Catalog = nil
	Editor.fragmentProps = nil
	Editor.reactions = nil
	Editor.list = nil
	collectgarbage()
end

function Editor:OnSearch(elementName, instanceId, eventName, args)
	runSearch(tostring(argument(args) or ""))
end

function Editor:OnTab(elementName, instanceId, eventName, args)
	selectTab(tonumber(argument(args)) or TAB_ANIMATION)
end

function Editor:OnScroll(elementName, instanceId, eventName, args)
	local step = (tonumber(argument(args)) or 0) > 0 and -SCROLL_ROWS or SCROLL_ROWS
	Editor.offset = clampOffset(Editor.offset + step)
	showRows()
end

-- The scrollbar was dragged or clicked: the row it names is the first shown.
function Editor:OnScrollTo(elementName, instanceId, eventName, args)
	Editor.offset = clampOffset(tonumber(argument(args)) or 0)
	showRows()
end

-- Adds value to list, or takes it out when it is there (same tells two entries apart).
local function toggle(list, value, entry, same)
	local index = indexOf(list, function(known) return same(known, value) end)
	if index then
		table.remove(list, index)
		return false
	end
	list[#list + 1] = entry
	return true
end

local function sameFragment(known, value)
	return known.fragment == value
end

local function sameValue(known, value)
	return known == value
end

-- A click on the reaction tab: on the Reaction section it picks the gesture's reaction or renames the gold one, on a
-- part section it changes that reaction, for every gesture that uses it.
local function pickReactionRow(result)
	local draft = Editor.draft
	if Editor.section == SECTION_REACTION then
		if result.action == "rename" then
			editedReaction().label = Editor.text
			editedReaction().edited = true
		else
			draft.reaction = result.choice == "reaction" and result.value or nil
		end
		return
	end
	local reaction = editedReaction()
	if reaction == nil then
		return
	end
	reaction.edited = true
	if Editor.section == SECTION_ANIMATION or Editor.section == SECTION_SEATED then
		local list = Editor.section == SECTION_ANIMATION and reaction.animations or reaction.seated
		toggle(list, result.value, { fragment = result.value, women = result.women }, sameFragment)
	elseif Editor.section == SECTION_BARKS then
		toggle(reaction.barks, result.value, result.value, sameValue)
	elseif Editor.section == SECTION_LINES then
		if toggle(reaction.lines, result.value, result.value, sameValue) then
			draft.voiceLabels[result.value] = result.label
			System.ExecuteCommand(SPEAK_COMMAND .. result.value)
		end
	elseif Editor.section == SECTION_REPUTATION then
		local clear = result.none or reaction.reputation == result.value
		reaction.reputation = not clear and result.value or nil
		reaction.change = not clear and result.change or 0
	else
		reaction.fight = result.count and { count = result.count, window = result.window } or nil
	end
end

-- An animation entry of the gesture from a picked row.
local function entryOf(row)
	return {
		fragment = row.value,
		partial = row.partial,
		tags = row.tags,
		seat = row.seat,
		horse = row.horse,
		option = row.option,
		set = row.set,
		clip = row.clip,
	}
end

-- The row of the fragment named name, or nil when Henry has no such fragment.
local function catalogFragment(name)
	System.ExecuteCommand(ANIMATION_LINE .. name)
	local line = System.GetCVar(ANIMATION_TEXT)
	return line and line ~= "" and fragmentRow(line) or nil
end

-- Picking an In named the game's way (X_In, XIn, X_Start, XStart) fills an empty Loop and Out with its X_Loop (or the
-- bare X, as PrayKneelingIn, PrayKneeling, PrayKneelingOut) and X_Out.
local function fillParts(name)
	for _, endings in ipairs(PART_ENDINGS) do
		local stem = string.sub(name, -#endings[1]) == endings[1] and string.sub(name, 1, -#endings[1] - 1)
		if stem and stem ~= "" then
			local candidates = { loop = { stem .. endings[2], stem }, outro = { stem .. endings[3] } }
			for list, names in pairs(candidates) do
				for _, candidate in ipairs(names) do
					local row = #Editor.draft[list] == 0 and catalogFragment(candidate)
					if row then
						Editor.draft[list][1] = entryOf(row)
					end
				end
			end
			return
		end
	end
end

function Editor:OnPick(elementName, instanceId, eventName, args)
	local result = listRow(Editor.list, (tonumber(argument(args)) or 0) + 1 + Editor.offset)
	if result == nil or result.notice then
		return
	end
	local draft = Editor.draft
	if result.timing == "hold" then
		draft.hold = not draft.hold or nil
	elseif result.timing == "duration" then
		draft.duration = result.seconds
	elseif Editor.tab == TAB_ANIMATION or Editor.tab == TAB_SEQUENCE then
		local list = postureList()
		local index = indexOf(list, function(animation) return animationKey(animation) == result.key end)
		if index then
			table.remove(list, index)
		else
			list[#list + 1] = entryOf(result)
			draft.label = draft.label or result.value
			-- An animation made only for a weapon or a torch sets the hands to take it out, unless they are set already.
			local needs = Wheel.HandVersions and Wheel.HandVersions[result.value] and Wheel.HandVersions[result.value].needs
			if needs and draft.hands == nil then
				draft.hands = needs
			end
			-- One the game's NPCs play holding a prop (a fishing rod) puts it in a hand the player left alone.
			for hand, prop in pairs(fragmentProps(result.value) or {}) do
				if draft.props[hand] == nil and not Editor.propsSet[hand] then
					draft.props[hand] = { model = prop.model, label = prop.label }
				end
			end
			if Editor.tab == TAB_SEQUENCE and part().list == "intro" and result.option == nil then
				fillParts(result.value)
			end
		end
	elseif Editor.tab == TAB_VOICE then
		local index = indexOf(draft.voice, function(file) return file == result.value end)
		if index then
			table.remove(draft.voice, index)
		else
			draft.voice[#draft.voice + 1] = result.value
			draft.voiceLabels[result.value] = result.label
			System.ExecuteCommand(SPEAK_COMMAND .. result.value)
		end
	elseif Editor.tab == TAB_REACTION then
		pickReactionRow(result)
	elseif result.prop then
		local prop = draft.props[result.hand]
		draft.props[result.hand] = not (prop and prop.model == result.prop) and { model = result.prop, label = result.label } or nil
		Editor.propsSet[result.hand] = true
	elseif result.use then
		Wheel.slots[draft.slot] = result.use
		Wheel.SaveSettings()
		Editor.Close()
		return
	elseif result.option then
		draft[result.option] = not optionOn(draft, result.option)
	elseif result.hands ~= nil then
		draft.hands = result.hands or nil
	end
	showSummary()
	showControls()
	showRows()
end

-- A section button: on the animation tab its posture's list is the one edited, with the filter on; on the reaction tab
-- its section shows.
function Editor:OnPosture(elementName, instanceId, eventName, args)
	local index = (tonumber(argument(args)) or 0) + 1
	if Editor.tab == TAB_REACTION then
		Editor.section = SECTION_LABELS[index] and index or Editor.section
		runSearch(Editor.text or "")
		return
	elseif Editor.tab == TAB_SEQUENCE then
		-- The parts, then Timing after them.
		Editor.part = index <= #PARTS + 1 and index or Editor.part
		runSearch(Editor.text or "")
		return
	elseif Editor.tab == TAB_NAME then
		-- The box holds the name on Options and a search on a hand, so it changes with the section.
		Editor.nameSection = NAME_SECTIONS[index] and index or Editor.nameSection
		local text = openedText(TAB_NAME)
		call("SetField", text)
		runSearch(text)
		return
	end
	local entry = POSTURES[index]
	if entry then
		Editor.posture = entry.id
		Editor.filter = true
		runSearch(Editor.text or "")
	end
end

-- The filter box: on the animation tab it shows only animations with a version for the posture or all of them; on the
-- Reaction section it deletes the gold custom reaction or puts a built-in one back as it came.
function Editor:OnFilter(elementName, instanceId, eventName, args)
	if Editor.tab == TAB_REACTION then
		local reaction = editedReaction()
		if reaction and isCustomReaction(reaction.id) then
			Editor.reactions[reaction.id] = nil
			Editor.removedReactions[reaction.id] = true
			table.remove(Editor.reactionOrder, indexOf(Editor.reactionOrder, function(id) return id == reaction.id end))
			Editor.draft.reaction = nil
		elseif reaction and Wheel.DefaultReaction(reaction.id) then
			local default = copyReaction(Wheel.DefaultReaction(reaction.id))
			default.edited = false
			Editor.reactions[reaction.id] = default
		end
		showSummary()
		runSearch(Editor.text or "")
		return
	end
	Editor.filter = not Editor.filter
	runSearch(Editor.text or "")
end

-- The Clear button: empties the list edited (the posture's animations, the voices, a reaction's part); on the Reaction
-- section it is New, a copy of the gold reaction (or of Friendly) that the gesture then uses.
function Editor:OnClear(elementName, instanceId, eventName, args)
	if Editor.tab == TAB_ANIMATION then
		Editor.draft[posture().list] = {}
	elseif Editor.tab == TAB_SEQUENCE and part() then
		Editor.draft[part().list] = {}
	elseif Editor.tab == TAB_VOICE then
		Editor.draft.voice = {}
	elseif Editor.tab == TAB_NAME and nameSection().hand then
		Editor.draft.props[nameSection().hand] = nil
		Editor.propsSet[nameSection().hand] = true
	elseif Editor.tab == TAB_REACTION and Editor.section == SECTION_REACTION then
		local number = 1
		while Editor.reactions[Wheel.CUSTOM_REACTION_PREFIX .. number] or Wheel.reactions[Wheel.CUSTOM_REACTION_PREFIX .. number] do
			number = number + 1
		end
		local made = copyReaction(editedReaction() or Editor.reactions[Editor.reactionOrder[1]])
		made.id = Wheel.CUSTOM_REACTION_PREFIX .. number
		made.label = string.format(NEW_REACTION_LABEL, number)
		made.edited = true
		Editor.reactions[made.id] = made
		Editor.reactionOrder[#Editor.reactionOrder + 1] = made.id
		Editor.removedReactions[made.id] = nil
		Editor.draft.reaction = made.id
		showSummary()
		runSearch(Editor.text or "")
		return
	elseif Editor.tab == TAB_REACTION and editedReaction() then
		local reaction = editedReaction()
		local field = Editor.section == SECTION_ANIMATION and "animations" or Editor.section == SECTION_SEATED and "seated" or Editor.section == SECTION_BARKS and "barks" or "lines"
		reaction[field] = {}
		reaction.edited = true
	end
	showSummary()
	showControls()
	showRows()
end

-- Hands the editor's reactions to the wheel: removed custom ones go, changed and custom ones replace the wheel's.
local function saveReactions()
	for id in pairs(Editor.removedReactions) do
		Wheel.RemoveReaction(id)
	end
	for _, id in ipairs(Editor.reactionOrder) do
		local reaction = Editor.reactions[id]
		if reaction.edited or isCustomReaction(id) then
			Wheel.RegisterReaction(reaction)
		elseif Wheel.reactions[id] and Wheel.reactions[id].edited then
			-- Reset: the built-in comes back as it came.
			Wheel.RegisterReaction(Wheel.DefaultReaction(id))
		end
	end
end

function Editor:OnSave(elementName, instanceId, eventName, args)
	local draft = Editor.draft
	saveReactions()
	local fragments = draftFragments()
	if #fragments == 0 then
		call("SetHelp", SAVE_NEEDS_ANIMATION)
		return
	end
	local saved = Wheel.Register({
		id = Wheel.CUSTOM_PREFIX .. draft.slot,
		label = draft.label or fragments[1],
		fragment = draft.animations,
		seated = draft.seated,
		mounted = draft.mounted,
		intro = draft.intro,
		loop = draft.loop,
		outro = draft.outro,
		hold = draft.hold,
		hands = draft.hands,
		duration = draft.duration,
		voice = draft.voice,
		reaction = draft.reaction,
		passThrough = draft.passThrough,
		lock = draft.lock,
		props = draft.props,
		slot = draft.slot,
	})
	if saved then
		Wheel.SaveSettings()
		Editor.Close()
	end
end

function Editor:OnCancel(elementName, instanceId, eventName, args)
	Editor.Close()
end

function Editor.Listen()
	if Editor.listening then
		return
	end
	Editor.listening = true
	for _, event in ipairs({ "OnSearch", "OnPick", "OnTab", "OnPosture", "OnFilter", "OnClear", "OnScroll", "OnScrollTo", "OnSave", "OnCancel" }) do
		UIAction.RegisterElementListener(Editor, ELEMENT, -1, event, event)
	end
end
