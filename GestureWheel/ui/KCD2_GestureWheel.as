// KCD2_GestureWheel - the gesture wheel (ActionScript 2, frame 1 of the movie).
//
// The movie owns the pointer: it turns mouse moves into a hovered slot and tells Lua only when that slot changes, so
// Lua never polls it. Lua owns what a slot means: it sends the labels (SetSlots, SetCenter) and acts on the events
// (OnReady, OnHover, OnPick, OnCancel, OnSettle). The ring is drawn with the drawing API, so it stays sharp at any resolution.

// The movie's root timeline. Functions reach their clips through it, whatever object they are called on.
var view:MovieClip = this;
// Scaleform's ActionScript extensions, on in every game movie. Mouse listeners get the button from them.
_global.gfxExtensions = true;

var SLOTS:Number = 8;
// The wheel's center on the 1920x1080 stage.
var CENTER_X:Number = 960;
var CENTER_Y:Number = 540;
// Inside INNER_RADIUS the wheel selects nothing, so releasing there cancels.
var INNER_RADIUS:Number = 130;
var OUTER_RADIUS:Number = 330;
var LABEL_RADIUS:Number = 230;
// A text field's box keeps room under the baseline, so a label sits this far above its anchor to look centered.
var LABEL_RISE:Number = 20;
// Half the gap between two slots, in degrees.
var GAP:Number = 1;
// Line segments per arc.
var ARC_STEPS:Number = 16;
var FILL:Number = 0x15110B;
var FILL_ALPHA:Number = 72;
var HOVER_FILL:Number = 0xB08A3C;
var HOVER_ALPHA:Number = 88;
var EDGE:Number = 0xC8A24A;
var EDGE_ALPHA:Number = 55;
var MOUSE_RIGHT:Number = 2;

// The hovered slot, -1 for none.
var hovered:Number = -1;

// The point at @p radius and @p degrees (0 at the right, clockwise, y down) around the wheel's center.
function pointX(radius:Number, degrees:Number):Number
{
	return Math.cos(degrees * Math.PI / 180) * radius;
}

function pointY(radius:Number, degrees:Number):Number
{
	return Math.sin(degrees * Math.PI / 180) * radius;
}

// Slot 0 is at the top and the slots run clockwise.
function slotAngle(slot:Number):Number
{
	return slot * 360 / SLOTS - 90;
}

function drawSector(clip:MovieClip, slot:Number, color:Number, alpha:Number):Void
{
	var half:Number = 180 / SLOTS - GAP;
	var start:Number = slotAngle(slot) - half;
	var sweep:Number = half * 2;
	var i:Number;
	clip.clear();
	clip.lineStyle(2, EDGE, EDGE_ALPHA);
	clip.beginFill(color, alpha);
	clip.moveTo(pointX(OUTER_RADIUS, start), pointY(OUTER_RADIUS, start));
	for (i = 1; i <= ARC_STEPS; i++)
	{
		clip.lineTo(pointX(OUTER_RADIUS, start + sweep * i / ARC_STEPS), pointY(OUTER_RADIUS, start + sweep * i / ARC_STEPS));
	}
	for (i = ARC_STEPS; i >= 0; i--)
	{
		clip.lineTo(pointX(INNER_RADIUS, start + sweep * i / ARC_STEPS), pointY(INNER_RADIUS, start + sweep * i / ARC_STEPS));
	}
	clip.endFill();
}

function slotClip(slot:Number):MovieClip
{
	return view["mc_Slot" + slot];
}

// Which slot the stage point (x, y) is over, -1 inside the hub.
function slotAt(x:Number, y:Number):Number
{
	var dx:Number = x - CENTER_X;
	var dy:Number = y - CENTER_Y;
	if (dx * dx + dy * dy < INNER_RADIUS * INNER_RADIUS)
	{
		return -1;
	}
	var degrees:Number = Math.atan2(dy, dx) * 180 / Math.PI + 90;
	var slot:Number = Math.round(degrees * SLOTS / 360) % SLOTS;
	return slot < 0 ? slot + SLOTS : slot;
}

function setHover(slot:Number):Void
{
	if (slot == hovered)
	{
		return;
	}
	if (hovered >= 0)
	{
		drawSector(slotClip(hovered), hovered, FILL, FILL_ALPHA);
	}
	if (slot >= 0)
	{
		drawSector(slotClip(slot), slot, HOVER_FILL, HOVER_ALPHA);
	}
	hovered = slot;
	fscommand("onHover", String(slot));
}

// Lua: the slot labels, joined with "|".
function fc_setSlots(labels:String):Void
{
	var names:Array = labels.split("|");
	for (var i:Number = 0; i < SLOTS; i++)
	{
		slotClip(i).mc_Label.t.text = names[i] == undefined ? "" : names[i];
	}
}

// Lua: the text in the hub.
function fc_setCenter(text:String):Void
{
	view.mc_Center.t.text = text;
}

// Lua: a gamepad stick points at this slot (-1 for none); the mouse can still take over by moving.
function fc_setHover(slot:String):Void
{
	setHover(Number(slot));
}

// Lua: the wheel opens. The pointer may already rest over a slot.
function fc_reset():Void
{
	setHover(slotAt(view._xmouse, view._ymouse));
}

// Lua: a gamepad stick direction was let go. After ms milliseconds the movie sends onSettle with the serial, and Lua
// points the slot again unless a newer stick event came. The wait runs here because no Lua timer runs while the game
// is paused, as it is while the wheel is open; the movie's frames do.
var settleDue:Number = -1;
var settleSerial:String = "";
function fc_settle(serial:String, ms:String):Void
{
	settleSerial = serial;
	settleDue = getTimer() + Number(ms);
}

for (var slot:Number = 0; slot < SLOTS; slot++)
{
	var clip:MovieClip = view.createEmptyMovieClip("mc_Slot" + slot, slot + 1);
	clip._x = CENTER_X;
	clip._y = CENTER_Y;
	drawSector(clip, slot, FILL, FILL_ALPHA);
	var label:MovieClip = clip.attachMovie("SlotLabel", "mc_Label", 1);
	label._x = pointX(LABEL_RADIUS, slotAngle(slot));
	label._y = pointY(LABEL_RADIUS, slotAngle(slot)) - LABEL_RISE;
}
var center:MovieClip = view.attachMovie("CenterLabel", "mc_Center", SLOTS + 1);
center._x = CENTER_X;
center._y = CENTER_Y - LABEL_RISE;

var pointer:Object = {};
pointer.onMouseMove = function():Void
{
	setHover(slotAt(view._xmouse, view._ymouse));
};
pointer.onMouseDown = function(button:Number):Void
{
	if (button == MOUSE_RIGHT)
	{
		fscommand("onCancel");
		return;
	}
	fscommand("onPick", String(hovered));
};
Mouse.addListener(pointer);

var settler:MovieClip = view.createEmptyMovieClip("mc_Settle", SLOTS + 2);
settler.onEnterFrame = function():Void
{
	if (settleDue >= 0 && getTimer() >= settleDue)
	{
		settleDue = -1;
		fscommand("onSettle", settleSerial);
	}
};

// An event sent from the first frame never reaches Lua, so the ready event waits for the next frame.
view.onEnterFrame = function():Void
{
	delete view.onEnterFrame;
	fscommand("onReady");
};
