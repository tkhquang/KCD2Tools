// KCD2_OblivionLockpicking - the tumbler lock view (ActionScript 2, frame 1 of the movie).
//
// The mod's C++ owns every rule and every timing, and this movie only places the lock's layers. The C++ calls update()
// on every frame of a lock. The layers are rendered textures in the mod's pak, loaded through img:// as the vanilla
// lockpicking view loads its own. LAYOUT, prepended at build time from ui/KCD2_OblivionLockpicking_layout.json, gives
// each layer's image size and offset and the lock's anchors, in 1080p stage pixels around the lock center, y down.

// The movie's root timeline. Functions reach their clips through it, whatever object they are called on.
var view:MovieClip = this;
// Scaleform's ActionScript extensions, on in every game movie. The keycap labels need textAutoSize from them, which
// shrinks a long label such as "Espacio" to fit its keycap.
_global.gfxExtensions = true;

var TEXTURES:String = "img://Libs/UI/Textures/KCD2_OblivionLockpicking/";
var TIER_NAMES:Array = ["common", "fancy"];
var TUMBLERS:Number = 5;
// The lock center on the 1920x1080 stage.
var CENTER_X:Number = 960;
var CENTER_Y:Number = 594;
// Shortest spring drawn. Below this length the tumbler hides it.
var SPRING_MIN:Number = 0.02;
// Highest tumbler height drawn: a tumbler in its pause sits a little above the top, as in Oblivion.
var MAX_HEIGHT:Number = 1.2;
// Frames without an update() after which the lock hides itself: the session can end without a last update, when a
// save loads in the middle of it or the mod reloads.
var IDLE_FRAMES:Number = 15;
var idle:Number = IDLE_FRAMES;
// The loaded plates: the tier (0 common, 1 fancy) and the front plate's grade (1 to TUMBLERS), -1 before the first.
var tier:Number = -1;
var grade:Number = -1;
var loader:MovieClipLoader = new MovieClipLoader();
// The header's texts, localization keys (text_ui_menus__oblivion_lockpicking.xml): "Tumblers set %1 of %2" and the
// lock's difficulty by its tumbler count, 1 to 5.
var HEADER_KEY:String = "@ui_oblivion_lockpicking_header";
var DIFFICULTY_KEY:String = "@ui_oblivion_lockpicking_difficulty_";
var header:String;
var difficulty:Array;
// The bars above and below the lock: their tops, in stage pixels from the lock's center, and their size.
var HEADER_Y:Number = -500;
var FOOTER_Y:Number = 196;
var ROW_HEIGHT:Number = 40;
var ROW_HALF_WIDTH:Number = 380;
// A text field's box keeps room under the baseline for descenders. A drop of this much centers the letters.
var TEXT_DROP:Number = 3;
// The control hint row: the game's own button icons between words. buttons.gfx is the movie the vanilla hints load,
// one frame per key, named after it. Icon widths are at its BUTTON_HEIGHT, as the vanilla hints space them.
// The vanilla movies ask for "buttons.swf", but from this movie only the .gfx name loads.
var BUTTONS:String = "buttons.gfx";
var BUTTON_HEIGHT:Number = 64;
var ICON_HEIGHT:Number = 32;
var ICON_WIDTH:Object = {escape: 83, enter: 106, pad_touch: 106};
var ICON_DEFAULT_WIDTH:Number = 52;
var ITEM_GAP:Number = 6;
// The name prefix of the row's icon holders, the clips a button movie is loaded into.
var ICON_PREFIX:String = "mc_Icon";
// The icons of the current row by holder name: the button movie's frame and the keycap label.
var icons:Object = {};
// One loader and one listener serve every icon. A loader and a closure per icon form a reference cycle through the
// closure's scope, which reference counts alone never free.
var buttonLoader:MovieClipLoader = new MovieClipLoader();
var buttonListener:Object = {};
buttonListener.onLoadInit = function(target:MovieClip):Void
{
	var icon:Object = icons[target._name];
	var button:MovieClip = target.mc_button;
	if (icon == undefined || button == undefined)
	{
		return;
	}
	button.gotoAndStop(icon.frame);
	if (icon.text != undefined)
	{
		button.tField.textAutoSize = "shrink";
		button.tField.text = icon.text;
	}
	button._xscale = button._yscale = ICON_HEIGHT / BUTTON_HEIGHT * 100;
	button.mc_holdIndicator._visible = false;
	button.mc_holdIcon._visible = false;
};
buttonLoader.addListener(buttonListener);
var SECTION_GAP:Number = 30;
// A breaking pick snaps this far behind its tip (pick image pixels, the tip at 0): the tip drops into the keyway, the
// shaft jerks and is drawn out, and a new pick slides in once the game hands one over. Times in seconds, lengths in
// stage pixels.
var BREAK_AT:Number = 70;
var TIP_GRAVITY:Number = 1400;
var TIP_DRIFT:Number = 40;
var TIP_SPIN:Number = 140;
var TIP_FADE:Number = 0.6;
var SNAP_SHAKE:Number = 4;
var SNAP_TIME:Number = 0.12;
var WITHDRAW_START:Number = 0.25;
var WITHDRAW_TIME:Number = 0.45;
var WITHDRAW_DISTANCE:Number = 300;
var INSERT_TIME:Number = 0.3;
// getTimer() when the pick broke and when a new one started in, -1 while neither runs.
var brokeAt:Number = -1;
var insertAt:Number = -1;

// A holder clip at (x, y) with an empty image clip inside, placed at the layer's offset and scaled to stage pixels.
function holder(parent:MovieClip, name:String, depth:Number, x:Number, y:Number, layer:String):MovieClip
{
	var clip:MovieClip = parent.createEmptyMovieClip(name, depth);
	clip._x = x;
	clip._y = y;
	var image:MovieClip = clip.createEmptyMovieClip("mc_Image", 1);
	var entry:Object = LAYOUT.layers[layer];
	image._x = entry.x;
	image._y = entry.y;
	image._xscale = image._yscale = LAYOUT.texture_scale * 100;
	return clip;
}

// A load over a clip's image does not free the texture already in it. unloadMovie() frees it, and it also cancels a
// load still on its way in. The image moves to the new layer's offset, because the front plates differ in size.
function load(clip:MovieClip, layer:String):Void
{
	var entry:Object = LAYOUT.layers[layer];
	clip.mc_Image.unloadMovie();
	clip.mc_Image._x = entry.x;
	clip.mc_Image._y = entry.y;
	loader.loadClip(TEXTURES + entry.file, clip.mc_Image);
}

// Frees the button movies loaded into row's icon holders before the row is removed. removeMovieClip() alone keeps the
// loaded movies and their textures alive, and leaves a load still on its way in with a target that no longer exists.
// Only the holders: unloadMovie() on a word label, a symbol of this movie, breaks the font of every label made after.
function unloadRow(row:MovieClip):Void
{
	for (var name in row)
	{
		if (name.indexOf(ICON_PREFIX) == 0)
		{
			row[name].unloadMovie();
		}
	}
}

// The layers that differ between locks: a fancy lock's iron is blackened and gilt where a common lock's is rusty, and
// the front plate grows richer with the lock's difficulty, its tumbler count, as the vanilla lock's rings do.
function loadPlates(nextTier:Number, nextGrade:Number):Void
{
	tier = nextTier;
	grade = nextGrade;
	var suffix:String = "_" + TIER_NAMES[tier];
	var lock:MovieClip = view.mc_Lock;
	load(lock.mc_Back, "back" + suffix);
	load(lock.mc_Front, "front" + suffix + "_" + grade);
	load(lock.mc_GearL, "gear" + suffix);
	load(lock.mc_GearR, "gear" + suffix);
}

function build():Void
{
	var lock:MovieClip = view.createEmptyMovieClip("mc_Lock", 1);
	lock._x = CENTER_X;
	lock._y = CENTER_Y;
	holder(lock, "mc_Back", 1, 0, 0, "back_common");
	for (var i:Number = 0; i < TUMBLERS; i++)
	{
		var x:Number = LAYOUT.chambers_x[i];
		load(holder(lock, "mc_Spring" + i, 10 + i, x, LAYOUT.chamber_top, "spring"), "spring");
		load(holder(lock, "mc_Pin" + i, 20 + i, x, LAYOUT.pin_rest_tip, "pin"), "pin");
	}
	// The pick turns about the keyway entry, so a kick lifts the tip and dips the handle. It slides inside that pivot.
	// It is drawn in two parts that meet where it breaks.
	var pivot:MovieClip = lock.createEmptyMovieClip("mc_Pick", 40);
	pivot._x = LAYOUT.keyway_entry_x;
	pivot._y = LAYOUT.pick_tip_y;
	var slide:MovieClip = pivot.createEmptyMovieClip("mc_Slide", 1);
	var pick:Object = LAYOUT.layers.pick;
	pickPart(slide, "mc_Shaft", 1, {xMin: pick.x - 1, yMin: pick.y - 1, xMax: -BREAK_AT, yMax: pick.y + pick.h + 1});
	pickPart(slide, "mc_Tip", 2, {xMin: -BREAK_AT, yMin: pick.y - 1, xMax: pick.x + pick.w + 1,
		yMax: pick.y + pick.h + 1});
	holder(lock, "mc_Front", 50, 0, 0, "front_common_1");
	// A gear's shadow is a layer of its own that turns with the gear about the shadow's centre, so it stays where the
	// light throws it.
	var shadow:Array = LAYOUT.layers.gear_shadow.centre;
	load(holder(lock, "mc_GearShadowL", 58, LAYOUT.gears[0][0] + shadow[0], LAYOUT.gears[0][1] + shadow[1],
		"gear_shadow"), "gear_shadow");
	load(holder(lock, "mc_GearShadowR", 59, LAYOUT.gears[1][0] + shadow[0], LAYOUT.gears[1][1] + shadow[1],
		"gear_shadow"), "gear_shadow");
	holder(lock, "mc_GearL", 60, LAYOUT.gears[0][0], LAYOUT.gears[0][1], "gear_common");
	holder(lock, "mc_GearR", 61, LAYOUT.gears[1][0], LAYOUT.gears[1][1], "gear_common");
	// The label clip (a symbol of the movie) holds a field "t" in the game's own font. Sized to its text, it is
	// centered on its bar as the hint row's words are.
	lock.attachMovie("HeaderLabel", "mc_Header", 71).t.autoSize = "center";
	bar(lock, "mc_Header_Bar", 70, {xMin: -ROW_HALF_WIDTH, yMin: HEADER_Y, xMax: ROW_HALF_WIDTH,
		yMax: HEADER_Y + ROW_HEIGHT});
	bar(lock, "mc_Footer_Bar", 72, {xMin: -ROW_HALF_WIDTH, yMin: FOOTER_Y, xMax: ROW_HALF_WIDTH,
		yMax: FOOTER_Y + ROW_HEIGHT});
}

// A localization key in the player's language: the game translates a key as it is written into a text field.
function translate(field:TextField, key:String):String
{
	field.text = key;
	return field.text;
}

// The header's texts in the player's language, read again at the start of every lock, because the player can change
// the language between locks while the movie stays loaded.
function translateHeader():Void
{
	var field:TextField = view.mc_Lock.mc_Header.t;
	header = translate(field, HEADER_KEY);
	difficulty = [""];
	for (var d:Number = 1; d <= TUMBLERS; d++)
	{
		difficulty.push(translate(field, DIFFICULTY_KEY + d));
	}
}

function fillBox(clip:MovieClip, box:Object, color:Number, alpha:Number):Void
{
	clip.beginFill(color, alpha);
	clip.moveTo(box.xMin, box.yMin);
	clip.lineTo(box.xMax, box.yMin);
	clip.lineTo(box.xMax, box.yMax);
	clip.lineTo(box.xMin, box.yMax);
	clip.lineTo(box.xMin, box.yMin);
	clip.endFill();
}

// A dark bar behind a row of text.
function bar(lock:MovieClip, name:String, depth:Number, box:Object):Void
{
	fillBox(lock.createEmptyMovieClip(name, depth), box, 0x0A0908, 62);
}

// The part of the pick image inside box (pick pixels, the tip at 0), masked so the parts meet without overlapping.
// The mask sits in the part, so it moves with it when the part flies off.
function pickPart(slide:MovieClip, name:String, depth:Number, box:Object):Void
{
	var part:MovieClip = slide.createEmptyMovieClip(name, depth);
	var image:MovieClip = holder(part, "mc_Part", 1, 0, 0, "pick");
	load(image, "pick");
	var cut:MovieClip = part.createEmptyMovieClip("mc_Cut", 2);
	fillBox(cut, box, 0xFFFFFF, 100);
	image.setMask(cut);
}

// A whole pick again, in place.
function mendPick():Void
{
	var slide:MovieClip = view.mc_Lock.mc_Pick.mc_Slide;
	slide.mc_Tip._x = slide.mc_Tip._y = slide.mc_Tip._rotation = 0;
	slide.mc_Tip._alpha = slide.mc_Shaft._alpha = 100;
	slide.mc_Tip._visible = slide.mc_Shaft._visible = true;
	brokeAt = insertAt = -1;
}

// Places the pick while it breaks (breaking != 0) and while a new one comes in. reach is its place at rest.
function animatePick(reach:Number, breaking:Number):Void
{
	var pick:MovieClip = view.mc_Lock.mc_Pick;
	var slide:MovieClip = pick.mc_Slide;
	var now:Number = getTimer();
	if (breaking != 0 && brokeAt < 0)
	{
		mendPick();
		brokeAt = now;
	}
	else if (breaking == 0 && brokeAt >= 0)
	{
		mendPick();
		insertAt = now;
	}
	if (brokeAt >= 0)
	{
		var t:Number = (now - brokeAt) / 1000;
		// The tip drops into the keyway behind the lock's front plate and turns on its way.
		slide.mc_Tip._x = TIP_DRIFT * t;
		slide.mc_Tip._y = TIP_GRAVITY * t * t / 2;
		slide.mc_Tip._rotation = TIP_SPIN * t;
		slide.mc_Tip._alpha = Math.max(0, 100 * (1 - t / TIP_FADE));
		// The shaft jerks as it snaps, then comes out of the keyway.
		pick._rotation = t < SNAP_TIME ? SNAP_SHAKE * Math.sin(t / SNAP_TIME * Math.PI * 3) : 0;
		var out:Number = Math.min(Math.max((t - WITHDRAW_START) / WITHDRAW_TIME, 0), 1);
		slide._x = reach - WITHDRAW_DISTANCE * out * out;
		slide.mc_Shaft._alpha = 100 * (1 - out);
		return;
	}
	if (insertAt >= 0)
	{
		var p:Number = Math.min((now - insertAt) / 1000 / INSERT_TIME, 1);
		slide._x = reach - WITHDRAW_DISTANCE * (1 - p) * (1 - p);
		if (p >= 1)
		{
			insertAt = -1;
		}
	}
}

// The control hint row under the lock, written by the mod for the player's keys and controller. Tabs separate its
// sections, and each section is a run of words and icons separated by spaces. An icon is "#<frame>" or
// "#<frame>:<keycap label>". A word is a localization key ("@ui_oblivion_lockpicking_hint_move"), which its text field
// shows translated.
function controls(row:String):Void
{
	var lock:MovieClip = view.mc_Lock;
	unloadRow(lock.mc_Controls);
	lock.mc_Controls.removeMovieClip();
	icons = {};
	var clip:MovieClip = lock.createEmptyMovieClip("mc_Controls", 74);
	var x:Number = 0;
	var depth:Number = 1;
	var sections:Array = row.split("\t");
	for (var s:Number = 0; s < sections.length; s++)
	{
		if (s > 0)
		{
			x += SECTION_GAP - ITEM_GAP;
		}
		var items:Array = sections[s].split(" ");
		var words:String = "";
		for (var i:Number = 0; i < items.length; i++)
		{
			var item:String = items[i];
			if (item.charAt(0) != "#")
			{
				words = words == "" ? item : words + " " + item;
				continue;
			}
			if (words != "")
			{
				x = placeWords(clip, depth++, x, words);
				words = "";
			}
			x = placeIcon(clip, depth++, x, item.substr(1));
		}
		if (words != "")
		{
			x = placeWords(clip, depth++, x, words);
		}
	}
	clip._x = -(x - ITEM_GAP) / 2;
	clip._y = FOOTER_Y + ROW_HEIGHT / 2;
}

// Words in the game's font, from x and centered on the row. Returns where the next item starts.
function placeWords(row:MovieClip, depth:Number, x:Number, words:String):Number
{
	var clip:MovieClip = row.attachMovie("WordLabel", "mc_Words" + depth, depth);
	clip.t.text = words;
	clip._x = x;
	clip._y = TEXT_DROP - clip.t._height / 2;
	return x + clip.t._width + ITEM_GAP;
}

// One button icon: the game's button movie loaded into a holder at x and shown at the frame named after the key. A
// keycap frame shows its label, which the game localizes ("@ui_key_space", or the letter of the keyboard layout).
function placeIcon(row:MovieClip, depth:Number, x:Number, icon:String):Number
{
	var colon:Number = icon.indexOf(":");
	var frame:String = colon < 0 ? icon : icon.substr(0, colon);
	var text:String = colon < 0 ? undefined : icon.substr(colon + 1);
	// The button art is centered on the holder's y.
	var holder:MovieClip = row.createEmptyMovieClip(ICON_PREFIX + depth, depth);
	holder._x = x;
	icons[holder._name] = {frame: frame, text: text};
	buttonLoader.loadClip(BUTTONS, holder);
	var width:Number = ICON_WIDTH[frame] != undefined ? ICON_WIDTH[frame] : ICON_DEFAULT_WIDTH;
	return x + width * ICON_HEIGHT / BUTTON_HEIGHT + ITEM_GAP;
}

// Turns the left gear to angle degrees and the right one the other way, each with its shadow.
function turnGears(angle:Number):Void
{
	var lock:MovieClip = view.mc_Lock;
	lock.mc_GearL._rotation = lock.mc_GearShadowL._rotation = angle;
	lock.mc_GearR._rotation = lock.mc_GearShadowR._rotation = -angle;
}

// Called by the mod at the start of every lock and after every show of the element. A hidden element runs no frames,
// so the movie cannot see a lock end by itself. The next update() reads the header texts again and mends the pick.
// The gears go back to rest, the only angle their light is drawn for.
function begin():Void
{
	view.mc_Lock._visible = false;
	turnGears(0);
}

// pick: pick position in tumbler units, 0 for the leftmost.
// kick: how far the pick tip is lifted, in pixels.
// h0..h4: tumbler heights, 0 at rest to 1 at the top.
// opening: 1 while the lock turns open, so the gears turn.
// breaking: 1 while the pick breaks after a wrong set. The pick snaps, and a new one slides in after.
// scale: size of the lock, 1 for 1080p.
// fancy: 0 for a common lock, 1 for a fancy one.
// done, total: tumblers the player set and tumblers the lock needs.
// showHeader, showFooter: 1 shows the bar above the lock and the control hints below it.
function update(pick:Number, kick:Number, h0:Number, h1:Number, h2:Number, h3:Number, h4:Number, opening:Number,
	breaking:Number, scale:Number, fancy:Number, done:Number, total:Number, showHeader:Number, showFooter:Number):Void
{
	var lock:MovieClip = view.mc_Lock;
	idle = 0;
	if (!lock._visible)
	{
		translateHeader();
		mendPick();
	}
	lock._visible = true;
	lock._xscale = lock._yscale = scale * 100;
	var plate:Number = Math.min(Math.max(total, 1), TUMBLERS);
	if (fancy != tier || plate != grade)
	{
		loadPlates(fancy, plate);
	}
	var heights:Array = [h0, h1, h2, h3, h4];
	for (var i:Number = 0; i < TUMBLERS; i++)
	{
		var h:Number = Math.min(Math.max(heights[i], 0), MAX_HEIGHT);
		lock["mc_Pin" + i]._y = LAYOUT.pin_rest_tip - LAYOUT.pin_travel * h;
		var spring:MovieClip = lock["mc_Spring" + i];
		spring._visible = 1 - h > SPRING_MIN;
		spring._yscale = (1 - h) * 100;
	}
	var reach:Number = LAYOUT.chambers_x[0] + (LAYOUT.chambers_x[1] - LAYOUT.chambers_x[0]) * pick - LAYOUT.keyway_entry_x;
	lock.mc_Pick.mc_Slide._x = reach;
	lock.mc_Pick._rotation = -Math.atan2(kick, reach) * 180 / Math.PI;
	animatePick(reach, breaking);
	lock.mc_Header.t.text = header.split("%1").join(String(done)).split("%2").join(String(total)) + "      " +
		difficulty[total];
	lock.mc_Header._y = HEADER_Y + ROW_HEIGHT / 2 + TEXT_DROP - lock.mc_Header.t._height / 2;
	lock.mc_Header._visible = lock.mc_Header_Bar._visible = showHeader != 0;
	lock.mc_Footer_Bar._visible = lock.mc_Controls._visible = showFooter != 0;
	if (opening != 0)
	{
		turnGears(lock.mc_GearL._rotation + 6);
	}
}

build();
view.mc_Lock._visible = false;
view.onEnterFrame = function():Void
{
	if (idle < IDLE_FRAMES && ++idle == IDLE_FRAMES)
	{
		view.mc_Lock._visible = false;
	}
};
stop();
