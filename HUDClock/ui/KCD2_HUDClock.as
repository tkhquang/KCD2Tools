// The HUD clock movie. The mod's C++ sends each part's style and place, the line texts and the hour through
// CFlashPlayer::Invoke, and the movie only lays them out. The parts are the dial (part 0) and four text lines (parts 1
// to 4). Each part sits with one of its nine points (its pivot) on one of the nine points of its target, the screen or
// another part, then moves by its offset. Everything scales with the screen height as the HUD does (1080p is 100%).
// A hidden part (an empty line, the dial when off) takes no room: what attaches to it sits where it would be.
//
// Each font is a label clip that scripts/build_gfx.py exports with one text field, "t", bound to that game font, so
// the game's fontconfig.xml maps it to the font of the player's language.
//
// The dial is the game's own art, loaded from its paks: the 24-hour sun and moon ring of the wait screen (12 under the
// sun at the top, 24 under the moon at the bottom), masked to the arc around the top and turned so the current hour
// sits under the wait screen's pointer. Nothing of it ships with the mod.

Stage.scaleMode = "noScale";
Stage.align = "TL";

var FONT_CLIPS:Array = ["Label_Regular", "Label_Bold", "Label_Light", "Label_Manuscript", "Label_Display"];
// The font size of every label field in the movie, in pixels. A clip scales from it to the requested size.
var BASE_SIZE:Number = 32;
// A text field draws its text this many of its own pixels in from its top-left corner.
var FIELD_GUTTER:Number = 2;
var REFERENCE_HEIGHT:Number = 1080;
// The shadow's offset down and right, in pixels of a 1080p screen.
var SHADOW_OFFSET:Number = 2;
var SHADOW_COLOR:Number = 0x000000;
var SHADOW_ALPHA:Number = 70;

var DIAL_PART:Number = 0;
var PART_COUNT:Number = 5;
var SCREEN_TARGET:Number = -1;

var DIAL_TEXTURE:String = "img://Libs/UI/Textures/SkipTime/skip_time_lunette.dds";
var POINTER_TEXTURE:String = "img://Libs/UI/Textures/SkipTime/skip_time_arrow.dds";
// The ring's texture is 1024 pixels square, centred on the dial's centre. Its painted band runs between these radii.
var TEXTURE_HALF:Number = 512;
var RING_INNER:Number = 366;
var RING_OUTER:Number = 489;
// The arc of the ring that shows, in degrees each side of the top: two hours either way.
var ARC_HALF_ANGLE:Number = 30;
// The pointer texture (32 x 64, tip up) hangs over the top of the band, tip down, this far into it.
var POINTER_WIDTH:Number = 32;
var POINTER_HEIGHT:Number = 64;
var POINTER_DEPTH:Number = 12;
// The pointer's size against the ring. At the ring's own scale it is a few pixels wide on a 1080p HUD.
var POINTER_SCALE:Number = 1.6;
// The arc's outline, in the ring texture's pixels: about 1.5 screen pixels at the default size. Its line hides the
// mask's hard edge and keeps the pale dawn and dusk ends apart from a bright sky.
var OUTLINE_THICKNESS:Number = 8;
var OUTLINE_COLOR:Number = 0x2B1D0E;
var OUTLINE_ALPHA:Number = 85;
// The dial's bounds around its centre: the pointer's top, the arc's lowest corner and its half width.
var DIAL_TOP:Number = -(RING_OUTER - POINTER_DEPTH + POINTER_HEIGHT * POINTER_SCALE);
var DIAL_BOTTOM:Number = -RING_INNER * Math.cos(ARC_HALF_ANGLE * Math.PI / 180);
var DIAL_HALF_WIDTH:Number = RING_OUTER * Math.sin(ARC_HALF_ANGLE * Math.PI / 180);

var view:MovieClip = this;
var screenWidth:Number = Stage.width;
var screenHeight:Number = Stage.height;

// One record per text line, index 1 to 4 (the part number).
var lines:Array = [null];
for (var n:Number = 1; n < PART_COUNT; ++n)
{
	var line:Object = {};
	line.holder = view.createEmptyMovieClip("line" + n, 10 + n);
	line.shade = null;
	line.label = null;
	line.font = -1;
	line.size = 22;
	line.color = 0xFFFFFF;
	line.opacity = 100;
	line.shadow = true;
	line.text = "";
	lines.push(line);
}

// Where each part sits: {target, point, pivot, x, y}.
var places:Array = [];
for (var m:Number = 0; m < PART_COUNT; ++m)
{
	places.push({target: SCREEN_TARGET, point: 0, pivot: 0, x: 0, y: 0});
}

var dialShow:Boolean = false;
var dialSize:Number = 40;
var dialOpacity:Number = 100;
var dialShadowOn:Boolean = true;
var loader:MovieClipLoader = new MovieClipLoader();
var dial:MovieClip = view.createEmptyMovieClip("dial", 10);
var dialShade:MovieClip = dial.createEmptyMovieClip("shade", 0);
var wheel:MovieClip = dial.createEmptyMovieClip("wheel", 1);
var ring:MovieClip = wheel.createEmptyMovieClip("ring", 1);
ring._x = -TEXTURE_HALF;
ring._y = -TEXTURE_HALF;
var arc:MovieClip = dial.createEmptyMovieClip("arc", 2);
var outline:MovieClip = dial.createEmptyMovieClip("outline", 3);
var pointer:MovieClip = dial.createEmptyMovieClip("pointer", 4);
var pointerImage:MovieClip = pointer.createEmptyMovieClip("image", 1);
pointerImage._x = -POINTER_WIDTH / 2;
pointer._y = -RING_OUTER + POINTER_DEPTH;
pointer._rotation = 180;
pointer._xscale = pointer._yscale = POINTER_SCALE * 100;
var dialLoaded:Boolean = false;

// The band between radii r0 and r1, from angle a0 to a1 in degrees (0 is the top, clockwise), as a filled shape of
// fillColor at fillAlpha, outlined when lineThickness is above 0.
function drawArc(target:MovieClip, r0:Number, r1:Number, a0:Number, a1:Number, fillColor:Number, fillAlpha:Number,
	lineThickness:Number):Void
{
	var steps:Number = 24;
	target.clear();
	if (lineThickness > 0)
	{
		target.lineStyle(lineThickness, OUTLINE_COLOR, OUTLINE_ALPHA);
	}
	target.beginFill(fillColor, fillAlpha);
	for (var i:Number = 0; i <= steps; ++i)
	{
		var a:Number = (a0 + (a1 - a0) * i / steps - 90) * Math.PI / 180;
		if (i == 0)
		{
			target.moveTo(r1 * Math.cos(a), r1 * Math.sin(a));
		}
		else
		{
			target.lineTo(r1 * Math.cos(a), r1 * Math.sin(a));
		}
	}
	for (var j:Number = steps; j >= 0; --j)
	{
		var b:Number = (a0 + (a1 - a0) * j / steps - 90) * Math.PI / 180;
		target.lineTo(r0 * Math.cos(b), r0 * Math.sin(b));
	}
	var c:Number = (a0 - 90) * Math.PI / 180;
	target.lineTo(r1 * Math.cos(c), r1 * Math.sin(c));
	target.endFill();
}

drawArc(arc, RING_INNER - 2, RING_OUTER + 2, -ARC_HALF_ANGLE, ARC_HALF_ANGLE, 0xFFFFFF, 100, 0);
drawArc(dialShade, RING_INNER, RING_OUTER, -ARC_HALF_ANGLE, ARC_HALF_ANGLE, SHADOW_COLOR, SHADOW_ALPHA, 0);
drawArc(outline, RING_INNER, RING_OUTER, -ARC_HALF_ANGLE, ARC_HALF_ANGLE, 0, 0, OUTLINE_THICKNESS);
wheel.setMask(arc);
dial._visible = false;

// Loads the dial's art on its first use, so a clock without the dial never reads it.
function loadDial():Void
{
	if (dialLoaded)
	{
		return;
	}
	dialLoaded = true;
	loader.loadClip(DIAL_TEXTURE, ring);
	loader.loadClip(POINTER_TEXTURE, pointerImage);
}

// Puts a label of font number f in place of line l's current one.
function buildLine(l:Object, f:Number):Void
{
	if (f < 0 || f >= FONT_CLIPS.length || isNaN(f))
	{
		f = 0;
	}
	l.font = f;
	if (l.label != null)
	{
		l.label.removeMovieClip();
		l.shade.removeMovieClip();
	}
	l.shade = l.holder.attachMovie(FONT_CLIPS[f], "shade", 1);
	l.label = l.holder.attachMovie(FONT_CLIPS[f], "label", 2);
}

// Columns and rows of the nine points: 0 left or top, 1 centre, 2 right or bottom.
function column(p:Number):Number
{
	return p % 3;
}

function row(p:Number):Number
{
	return Math.floor(p / 3);
}

function layout():Void
{
	var screenScale:Number = screenHeight / REFERENCE_HEIGHT;
	var widths:Array = [];
	var heights:Array = [];

	// The dial's size, or none while it is off.
	var dialScale:Number = dialSize / (DIAL_BOTTOM - DIAL_TOP) * screenScale;
	dial._visible = dialShow;
	widths[DIAL_PART] = dialShow ? DIAL_HALF_WIDTH * 2 * dialScale : 0;
	heights[DIAL_PART] = dialShow ? (DIAL_BOTTOM - DIAL_TOP) * dialScale : 0;

	// Each line's text, style and size. An empty line takes no room.
	for (var i:Number = 1; i < PART_COUNT; ++i)
	{
		var l:Object = lines[i];
		var shown:Boolean = l.label != null && l.text != "";
		l.holder._visible = shown;
		if (!shown)
		{
			widths[i] = 0;
			heights[i] = 0;
			continue;
		}
		var scale:Number = l.size / BASE_SIZE * screenScale;
		l.label.t.text = l.text;
		l.shade.t.text = l.text;
		l.label.t.textColor = l.color;
		l.shade.t.textColor = SHADOW_COLOR;
		l.label._xscale = l.label._yscale = l.shade._xscale = l.shade._yscale = scale * 100;
		l.holder._alpha = l.opacity;
		l.shade._alpha = SHADOW_ALPHA;
		l.shade._visible = l.shadow;
		l.scale = scale;
		widths[i] = l.label.t.textWidth * scale;
		heights[i] = l.label.t.textHeight * scale;
	}

	// Each part's top-left corner, from its target's box. The C++ side breaks attachment loops, and the depth limit
	// keeps a loop that slipped through from recursing.
	var xs:Array = [];
	var ys:Array = [];
	var placeOf:Function = function(p:Number, depth:Number):Void
	{
		if (xs[p] != undefined)
		{
			return;
		}
		var pl:Object = places[p];
		var tx:Number = 0;
		var ty:Number = 0;
		var tw:Number = screenWidth;
		var th:Number = screenHeight;
		if (pl.target >= 0 && pl.target < PART_COUNT && pl.target != p && depth < PART_COUNT)
		{
			placeOf(pl.target, depth + 1);
			tx = xs[pl.target];
			ty = ys[pl.target];
			tw = widths[pl.target];
			th = heights[pl.target];
		}
		xs[p] = tx + column(pl.point) / 2 * tw + pl.x * screenScale - column(pl.pivot) / 2 * widths[p];
		ys[p] = ty + row(pl.point) / 2 * th + pl.y * screenScale - row(pl.pivot) / 2 * heights[p];
	};
	for (var p:Number = 0; p < PART_COUNT; ++p)
	{
		placeOf(p, 0);
	}

	if (dialShow)
	{
		// The shadow moves as far as the text's, in screen pixels, whatever the dial's size.
		dialShade._visible = dialShadowOn;
		dialShade._x = dialShade._y = SHADOW_OFFSET * screenScale / dialScale;
		dial._xscale = dial._yscale = dialScale * 100;
		dial._alpha = dialOpacity;
		dial._x = Math.round(xs[DIAL_PART] + DIAL_HALF_WIDTH * dialScale);
		dial._y = Math.round(ys[DIAL_PART] - DIAL_TOP * dialScale);
	}
	for (var k:Number = 1; k < PART_COUNT; ++k)
	{
		var t:Object = lines[k];
		if (!t.holder._visible)
		{
			continue;
		}
		// The field draws its text a gutter in from its corner, so the corner goes that far up and left.
		var lx:Number = xs[k] - FIELD_GUTTER * t.scale;
		var ly:Number = ys[k] - FIELD_GUTTER * t.scale;
		t.label._x = Math.round(lx);
		t.label._y = Math.round(ly);
		t.shade._x = Math.round(lx + SHADOW_OFFSET * screenScale);
		t.shade._y = Math.round(ly + SHADOW_OFFSET * screenScale);
	}
}

// i: line part 1 to 4, f: font number, s: text height in 1080p pixels, c: RRGGBB, a: opacity 0 to 100,
// sh: 1 for the shadow.
function setLineStyle(i:Number, f:Number, s:Number, c:Number, a:Number, sh:Number):Void
{
	var l:Object = lines[i];
	if (l == undefined || l == null)
	{
		return;
	}
	if (f != l.font)
	{
		buildLine(l, f);
	}
	l.size = s;
	l.color = c;
	l.opacity = a;
	l.shadow = sh != 0;
	layout();
}

// show: 1 to show the dial, s: its height in 1080p pixels, a: opacity 0 to 100, sh: 1 for its shadow.
function setDialStyle(show:Number, s:Number, a:Number, sh:Number):Void
{
	dialShow = show != 0;
	dialSize = s;
	dialOpacity = a;
	dialShadowOn = sh != 0;
	if (dialShow)
	{
		loadDial();
	}
	layout();
}

// p: part (0 the dial, 1 to 4 a line), target: -1 the screen or a part, point and pivot: point numbers (TopLeft 0 to
// BottomRight 8), x and y: offsets right and down in 1080p pixels.
function setPlace(p:Number, target:Number, point:Number, pivot:Number, x:Number, y:Number):Void
{
	if (p < 0 || p >= PART_COUNT)
	{
		return;
	}
	places[p] = {
		target: target,
		point: (point >= 0 && point <= 8) ? point : 0,
		pivot: (pivot >= 0 && pivot <= 8) ? pivot : 0,
		x: x,
		y: y
	};
	layout();
}

function setTexts(t1:String, t2:String, t3:String, t4:String):Void
{
	lines[1].text = t1;
	lines[2].text = t2;
	lines[3].text = t3;
	lines[4].text = t4;
	layout();
}

// h: the game hour, 0 to 24 with the minutes as a fraction. The ring turns so h sits at the top, under the pointer.
function setHour(h:Number):Void
{
	wheel._rotation = 180 - h * 15;
}

// The engine's resize call, as the HUD's own movie takes it.
function cry_onResize(width:Number, height:Number, newScreenWidth:Number, newScreenHeight:Number):Void
{
	screenWidth = newScreenWidth;
	screenHeight = newScreenHeight;
	layout();
}

var resizeListener:Object = {};
resizeListener.onResize = function():Void
{
	screenWidth = Stage.width;
	screenHeight = Stage.height;
	layout();
};
Stage.addListener(resizeListener);

for (var q:Number = 1; q < PART_COUNT; ++q)
{
	buildLine(lines[q], 0);
}
stop();
