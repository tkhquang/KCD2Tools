// KCD2_GestureEditor - the wheel's picker (ActionScript 2, frame 1 of the movie).
//
// The movie owns the search box, the tabs, the posture buttons, the filter box, the Clear button, the list and the
// buttons below it. It sends Lua what the player does: the text on every change (OnSearch), the tab, posture, row or
// button clicked, the filter box and Clear, the mouse wheel (OnScroll) and the row the scrollbar is dragged to
// (OnScrollTo). Lua owns what everything means: it answers with SetTitle, SetTabs, SetTab, SetPostures, SetFilter,
// SetClear, SetHelp, SetResults, SetScroll, SetSummary and SetField. Esc or the right mouse button cancels.

var view:MovieClip = this;
// Scaleform's ActionScript extensions, on in every game movie. Mouse listeners get the button from them.
_global.gfxExtensions = true;

var PANEL_X:Number = 360;
var PANEL_Y:Number = 40;
var PANEL_WIDTH:Number = 1200;
var PANEL_HEIGHT:Number = 1000;
var PANEL_FILL:Number = 0x15110B;
var PANEL_ALPHA:Number = 94;
var EDGE:Number = 0xC8A24A;
var BOX_FILL:Number = 0x2A2115;
var HOVER_FILL:Number = 0xB08A3C;
var HOVER_ALPHA:Number = 60;
var ACTIVE_ALPHA:Number = 90;
var TEXT:Number = 0xE8DCC4;
var CHOSEN_TEXT:Number = 0xF2C25A;
var NOTE_TEXT:Number = 0x9C8F78;
var PADDING:Number = 24;
var INNER_X:Number = PANEL_X + PADDING;
var INNER_WIDTH:Number = PANEL_WIDTH - PADDING * 2;
var TEXT_INSET:Number = 16;
var TITLE_Y:Number = PANEL_Y + 16;
var TABS:Number = 5;
var TAB_Y:Number = PANEL_Y + 66;
var TAB_HEIGHT:Number = 48;
var TAB_WIDTH:Number = INNER_WIDTH / TABS;
// The section buttons under the tabs, after a title: the postures on the animation tab, the parts on the sequence tab,
// the reaction's parts on the reaction tab and the options and hands on the last tab. Lua sends the title, up to
// POSTURE_MAX labels and the one picked.
var POSTURE_MAX:Number = 7;
var POSTURE_Y:Number = TAB_Y + TAB_HEIGHT + 12;
var POSTURE_HEIGHT:Number = 42;
var POSTURE_LABEL_WIDTH:Number = 150;
var POSTURE_X:Number = INNER_X + POSTURE_LABEL_WIDTH;
var POSTURE_SPAN:Number = INNER_WIDTH - POSTURE_LABEL_WIDTH;
var HELP_Y:Number = POSTURE_Y + POSTURE_HEIGHT + 8;
var HELP_HEIGHT:Number = 36;
// The search box, then the filter box and the Clear button on its right.
var SEARCH_Y:Number = HELP_Y + HELP_HEIGHT + 6;
var SEARCH_HEIGHT:Number = 54;
var CLEAR_WIDTH:Number = 150;
var FILTER_WIDTH:Number = 300;
var CONTROL_GAP:Number = 12;
var CLEAR_X:Number = INNER_X + INNER_WIDTH - CLEAR_WIDTH;
var FILTER_X:Number = CLEAR_X - CONTROL_GAP - FILTER_WIDTH;
var SEARCH_WIDTH:Number = FILTER_X - CONTROL_GAP - INNER_X;
var ROWS:Number = 12;
var ROW_HEIGHT:Number = 44;
var ROWS_Y:Number = SEARCH_Y + SEARCH_HEIGHT + 10;
// The scrollbar sits right of the rows, as tall as all of them. A wider strip around it takes the clicks.
var SCROLL_WIDTH:Number = 14;
var SCROLL_GAP:Number = 10;
var SCROLL_X:Number = INNER_X + INNER_WIDTH - SCROLL_WIDTH;
var ROWS_WIDTH:Number = INNER_WIDTH - SCROLL_WIDTH - SCROLL_GAP;
// A row's note sits right in the row; its name stops short of it while there are notes.
var NOTE_RIGHT:Number = INNER_X + ROWS_WIDTH - TEXT_INSET;
var NAME_WIDTH_WITH_NOTES:Number = 640;
var NAME_WIDTH:Number = ROWS_WIDTH - TEXT_INSET * 2;
var TRACK_HEIGHT:Number = ROWS * ROW_HEIGHT;
var MIN_THUMB:Number = 40;
var THUMB_ALPHA:Number = 85;
var SUMMARY_Y:Number = ROWS_Y + ROWS * ROW_HEIGHT + 10;
var BUTTON_Y:Number = SUMMARY_Y + 50;
var BUTTON_HEIGHT:Number = 50;
var BUTTON_WIDTH:Number = 270;
var SAVE_X:Number = PANEL_X + PANEL_WIDTH - PADDING * 2 - BUTTON_WIDTH * 2;
var CANCEL_X:Number = PANEL_X + PANEL_WIDTH - PADDING - BUTTON_WIDTH;
var LABEL_RISE:Number = 4;
var MOUSE_RIGHT:Number = 2;
var KEY_ESCAPE:Number = 27;
var LINE_SEPARATOR:String = "\n";
var LIST_SEPARATOR:String = "|";
var NOTE_SEPARATOR:String = "\t";
// A result line starts with this when the gesture has it, and with a space when not.
var CHOSEN_MARK:String = "*";
var NONE:Number = -1;

var hovered:Number = NONE;
var rowCount:Number = 0;
var ignoreChange:Boolean = false;
// The first row shown and how many rows the list has, from Lua; while the thumb is dragged, where it was grabbed.
var scrollOffset:Number = 0;
var scrollTotal:Number = 0;
var dragging:Boolean = false;
var grab:Number = 0;
// Which controls Lua shows: the posture buttons, the filter box and the Clear button.
var posturesShown:Boolean = false;
var postureCount:Number = 0;
var postureWidth:Number = 0;
var filterShown:Boolean = false;
var clearShown:Boolean = false;

function fillRect(clip:MovieClip, x:Number, y:Number, width:Number, height:Number, color:Number, alpha:Number):Void
{
	clip.beginFill(color, alpha);
	clip.moveTo(x, y);
	clip.lineTo(x + width, y);
	clip.lineTo(x + width, y + height);
	clip.lineTo(x, y + height);
	clip.lineTo(x, y);
	clip.endFill();
}

var panel:MovieClip = view.createEmptyMovieClip("mc_Panel", 1);
panel.lineStyle(2, EDGE, 80);
fillRect(panel, PANEL_X, PANEL_Y, PANEL_WIDTH, PANEL_HEIGHT, PANEL_FILL, PANEL_ALPHA);
panel.lineStyle(1, EDGE, 60);
fillRect(panel, INNER_X, SEARCH_Y, SEARCH_WIDTH, SEARCH_HEIGHT, BOX_FILL, 100);
fillRect(panel, SAVE_X, BUTTON_Y, BUTTON_WIDTH, BUTTON_HEIGHT, BOX_FILL, 100);
fillRect(panel, CANCEL_X, BUTTON_Y, BUTTON_WIDTH, BUTTON_HEIGHT, BOX_FILL, 100);
for (var tab:Number = 0; tab < TABS; tab++)
{
	fillRect(panel, INNER_X + tab * TAB_WIDTH, TAB_Y, TAB_WIDTH, TAB_HEIGHT, BOX_FILL, 100);
}

// The controls Lua can hide: the posture boxes, the filter box and the Clear button.
var postureBoxes:MovieClip = view.createEmptyMovieClip("mc_PostureBoxes", 2);
var filterBox:MovieClip = view.createEmptyMovieClip("mc_FilterBox", 3);
filterBox.lineStyle(1, EDGE, 60);
fillRect(filterBox, FILTER_X, SEARCH_Y, FILTER_WIDTH, SEARCH_HEIGHT, BOX_FILL, 100);
var clearBox:MovieClip = view.createEmptyMovieClip("mc_ClearBox", 4);
clearBox.lineStyle(1, EDGE, 60);
fillRect(clearBox, CLEAR_X, SEARCH_Y, CLEAR_WIDTH, SEARCH_HEIGHT, BOX_FILL, 100);

var activeTab:MovieClip = view.createEmptyMovieClip("mc_ActiveTab", 5);
fillRect(activeTab, 0, 0, TAB_WIDTH, TAB_HEIGHT, HOVER_FILL, ACTIVE_ALPHA);
activeTab._y = TAB_Y;

var activePosture:MovieClip = view.createEmptyMovieClip("mc_ActivePosture", 6);
activePosture._y = POSTURE_Y;

var highlight:MovieClip = view.createEmptyMovieClip("mc_Highlight", 7);
fillRect(highlight, 0, 0, ROWS_WIDTH, ROW_HEIGHT, HOVER_FILL, HOVER_ALPHA);
highlight._x = INNER_X;
highlight._visible = false;

var title:MovieClip = view.attachMovie("TitleText", "mc_Title", 8);
title._x = INNER_X;
title._y = TITLE_Y;
title.t.text = "";

var summary:MovieClip = view.attachMovie("ResultRow", "mc_Summary", 9);
summary._x = INNER_X;
summary._y = SUMMARY_Y;
summary.t.text = "";

var help:MovieClip = view.attachMovie("HelpText", "mc_Help", 10);
help._x = INNER_X;
help._y = HELP_Y;
help.t.text = "";
help.t.textColor = NOTE_TEXT;

var search:MovieClip = view.attachMovie("SearchField", "mc_Search", 11);
search._x = INNER_X + TEXT_INSET;
search._y = SEARCH_Y + 6;
var field:TextField = search.t;
field.type = "input";
field.selectable = true;
field.text = "";
field._width = SEARCH_WIDTH - TEXT_INSET * 2;
field.onChanged = function():Void
{
	if (!ignoreChange)
	{
		fscommand("onSearch", field.text);
	}
};

var saveLabel:MovieClip = view.attachMovie("ButtonText", "mc_Save", 12);
saveLabel._x = SAVE_X + BUTTON_WIDTH / 2;
saveLabel._y = BUTTON_Y + LABEL_RISE;
saveLabel.t.text = "Save";
var cancelLabel:MovieClip = view.attachMovie("ButtonText", "mc_Cancel", 13);
cancelLabel._x = CANCEL_X + BUTTON_WIDTH / 2;
cancelLabel._y = BUTTON_Y + LABEL_RISE;
cancelLabel.t.text = "Cancel";

var filterLabel:MovieClip = view.attachMovie("ButtonText", "mc_Filter", 14);
filterLabel._x = FILTER_X + FILTER_WIDTH / 2;
filterLabel._y = SEARCH_Y + LABEL_RISE + 4;
filterLabel.t.text = "";
var clearLabel:MovieClip = view.attachMovie("ButtonText", "mc_Clear", 15);
clearLabel._x = CLEAR_X + CLEAR_WIDTH / 2;
clearLabel._y = SEARCH_Y + LABEL_RISE + 4;
clearLabel.t.text = "";

var postureTitle:MovieClip = view.attachMovie("HelpText", "mc_PostureTitle", 16);
postureTitle._x = INNER_X;
postureTitle._y = POSTURE_Y + 6;
postureTitle.t.text = "";

var tabLabels:Array = [];
for (var index:Number = 0; index < TABS; index++)
{
	var label:MovieClip = view.attachMovie("ButtonText", "mc_Tab" + index, 20 + index);
	label._x = INNER_X + (index + 0.5) * TAB_WIDTH;
	label._y = TAB_Y + LABEL_RISE;
	label.t.text = "";
	tabLabels.push(label);
}

var postureLabels:Array = [];
for (var posture:Number = 0; posture < POSTURE_MAX; posture++)
{
	var postureLabel:MovieClip = view.attachMovie("ButtonText", "mc_Posture" + posture, 30 + posture);
	postureLabel._y = POSTURE_Y + 2;
	postureLabel.t.text = "";
	postureLabels.push(postureLabel);
}

var track:MovieClip = view.createEmptyMovieClip("mc_Track", 40);
track.lineStyle(1, EDGE, 60);
fillRect(track, SCROLL_X, ROWS_Y, SCROLL_WIDTH, TRACK_HEIGHT, BOX_FILL, 100);
track._visible = false;
var thumb:MovieClip = view.createEmptyMovieClip("mc_Thumb", 41);
thumb._x = SCROLL_X;
thumb._visible = false;

var rows:Array = [];
var notes:Array = [];
for (var line:Number = 0; line < ROWS; line++)
{
	var row:MovieClip = view.attachMovie("ResultRow", "mc_Row" + line, 50 + line);
	row._x = INNER_X + TEXT_INSET;
	row._y = ROWS_Y + line * ROW_HEIGHT + 4;
	row.t.text = "";
	rows.push(row);
	var note:MovieClip = view.attachMovie("ResultNote", "mc_Note" + line, 70 + line);
	note._x = NOTE_RIGHT;
	note._y = ROWS_Y + line * ROW_HEIGHT + 8;
	note.t.text = "";
	note.t.textColor = NOTE_TEXT;
	notes.push(note);
}

function inside(x:Number, y:Number, left:Number, top:Number, width:Number, height:Number):Boolean
{
	return x >= left && x < left + width && y >= top && y < top + height;
}

function rowAt(x:Number, y:Number):Number
{
	if (!inside(x, y, INNER_X, ROWS_Y, ROWS_WIDTH, ROWS * ROW_HEIGHT))
	{
		return NONE;
	}
	var at:Number = Math.floor((y - ROWS_Y) / ROW_HEIGHT);
	return at < rowCount ? at : NONE;
}

function tabAt(x:Number, y:Number):Number
{
	return inside(x, y, INNER_X, TAB_Y, INNER_WIDTH, TAB_HEIGHT) ? Math.floor((x - INNER_X) / TAB_WIDTH) : NONE;
}

function postureAt(x:Number, y:Number):Number
{
	if (!posturesShown || !inside(x, y, POSTURE_X, POSTURE_Y, POSTURE_SPAN, POSTURE_HEIGHT))
	{
		return NONE;
	}
	return Math.floor((x - POSTURE_X) / postureWidth);
}

function scrolls():Boolean
{
	return scrollTotal > ROWS;
}

function onScrollbar(x:Number, y:Number):Boolean
{
	return scrolls() && inside(x, y, SCROLL_X - SCROLL_GAP, ROWS_Y, SCROLL_WIDTH + SCROLL_GAP, TRACK_HEIGHT);
}

function thumbHeight():Number
{
	return Math.max(MIN_THUMB, TRACK_HEIGHT * ROWS / scrollTotal);
}

function placeThumb():Void
{
	track._visible = scrolls();
	thumb._visible = scrolls();
	if (!scrolls())
	{
		return;
	}
	var height:Number = thumbHeight();
	thumb.clear();
	fillRect(thumb, 0, 0, SCROLL_WIDTH, height, EDGE, THUMB_ALPHA);
	thumb._y = ROWS_Y + (TRACK_HEIGHT - height) * scrollOffset / (scrollTotal - ROWS);
}

// The first row for the thumb's top at y less where it was grabbed.
function offsetAt(y:Number):Number
{
	var fraction:Number = (y - grab - ROWS_Y) / (TRACK_HEIGHT - thumbHeight());
	return Math.round(Math.max(0, Math.min(1, fraction)) * (scrollTotal - ROWS));
}

function scrollTo(offset:Number):Void
{
	if (offset != scrollOffset)
	{
		scrollOffset = offset;
		placeThumb();
		fscommand("onScrollTo", String(offset));
	}
}

function setHover(row:Number):Void
{
	hovered = row;
	highlight._visible = row != NONE;
	highlight._y = ROWS_Y + row * ROW_HEIGHT;
}

// A click takes the keyboard from the box after the handlers have run, so it goes back a frame later.
function refocus():Void
{
	view.onEnterFrame = function():Void
	{
		delete view.onEnterFrame;
		Selection.setFocus(field);
		Selection.setSelection(field.text.length, field.text.length);
	};
}

function fc_setTitle(text:String):Void
{
	title.t.text = text;
}

function fc_setTabs(labels:String):Void
{
	var names:Array = labels.split(LIST_SEPARATOR);
	for (var at:Number = 0; at < TABS; at++)
	{
		tabLabels[at].t.text = names[at];
	}
}

function fc_setTab(tab:Number):Void
{
	activeTab._x = INNER_X + tab * TAB_WIDTH;
}

// "title|label|...|label|active": the section buttons and the one picked, or "" to hide them.
function fc_setPostures(state:String):Void
{
	var parts:Array = state.length > 0 ? state.split(LIST_SEPARATOR) : [];
	postureCount = Math.min(Math.max(parts.length - 2, 0), POSTURE_MAX);
	posturesShown = postureCount > 0;
	postureWidth = posturesShown ? POSTURE_SPAN / postureCount : 0;
	postureBoxes.clear();
	activePosture.clear();
	postureTitle._visible = posturesShown;
	postureTitle.t.text = posturesShown ? parts[0] : "";
	if (posturesShown)
	{
		postureBoxes.lineStyle(1, EDGE, 60);
		for (var box:Number = 0; box < postureCount; box++)
		{
			fillRect(postureBoxes, POSTURE_X + box * postureWidth, POSTURE_Y, postureWidth - CONTROL_GAP, POSTURE_HEIGHT, BOX_FILL, 100);
		}
		fillRect(activePosture, 0, 0, postureWidth - CONTROL_GAP, POSTURE_HEIGHT, HOVER_FILL, ACTIVE_ALPHA);
		activePosture._x = POSTURE_X + Number(parts[parts.length - 1]) * postureWidth;
	}
	for (var at:Number = 0; at < POSTURE_MAX; at++)
	{
		var shown:Boolean = at < postureCount;
		postureLabels[at]._visible = shown;
		postureLabels[at]._x = POSTURE_X + at * postureWidth + (postureWidth - CONTROL_GAP) / 2;
		postureLabels[at].t.text = shown ? parts[at + 1] : "";
	}
}

// The filter box's label, or "" to hide it.
function fc_setFilter(label:String):Void
{
	filterShown = label.length > 0;
	filterBox._visible = filterShown;
	filterLabel.t.text = label;
}

// The Clear button's label, or "" to hide it.
function fc_setClear(label:String):Void
{
	clearShown = label.length > 0;
	clearBox._visible = clearShown;
	clearLabel.t.text = label;
}

function fc_setHelp(text:String):Void
{
	help.t.text = text;
}

// One line per row: CHOSEN_MARK or a space, the name, and after NOTE_SEPARATOR an optional note.
function fc_setResults(results:String):Void
{
	var lines:Array = results.length > 0 ? results.split(LINE_SEPARATOR) : [];
	rowCount = Math.min(lines.length, ROWS);
	var withNotes:Boolean = false;
	for (var at:Number = 0; at < rowCount; at++)
	{
		if (lines[at].indexOf(NOTE_SEPARATOR) >= 0)
		{
			withNotes = true;
		}
	}
	for (var shown:Number = 0; shown < ROWS; shown++)
	{
		var text:String = shown < rowCount ? lines[shown] : " ";
		var chosen:Boolean = text.charAt(0) == CHOSEN_MARK;
		var parts:Array = text.substr(1).split(NOTE_SEPARATOR);
		rows[shown].t._width = withNotes ? NAME_WIDTH_WITH_NOTES : NAME_WIDTH;
		rows[shown].t.text = shown < rowCount ? parts[0] : "";
		rows[shown].t.textColor = chosen ? CHOSEN_TEXT : TEXT;
		notes[shown].t.text = shown < rowCount && parts.length > 1 ? parts[1] : "";
	}
	setHover(rowAt(view._xmouse, view._ymouse));
}

// "offset|total": the first row shown and how many the list has.
function fc_setScroll(state:String):Void
{
	var parts:Array = state.split(LIST_SEPARATOR);
	scrollOffset = Number(parts[0]);
	scrollTotal = Number(parts[1]);
	placeThumb();
}

function fc_setSummary(text:String):Void
{
	summary.t.text = text;
}

// Puts text in the box without it counting as a search, and gives the box the keyboard.
function fc_setField(text:String):Void
{
	ignoreChange = true;
	field.text = text;
	ignoreChange = false;
	refocus();
}

var pointer:Object = new Object();
pointer.onMouseMove = function():Void
{
	if (dragging)
	{
		scrollTo(offsetAt(view._ymouse));
		return;
	}
	setHover(rowAt(view._xmouse, view._ymouse));
};
pointer.onMouseDown = function(button:Number):Void
{
	if (button == MOUSE_RIGHT)
	{
		fscommand("onCancel");
		return;
	}
	var x:Number = view._xmouse;
	var y:Number = view._ymouse;
	var tab:Number = tabAt(x, y);
	var posture:Number = postureAt(x, y);
	if (onScrollbar(x, y))
	{
		// On the thumb it moves from where it was grabbed; on the track its middle jumps to the pointer.
		var top:Number = thumb._y;
		grab = y >= top && y < top + thumbHeight() ? y - top : thumbHeight() / 2;
		dragging = true;
		scrollTo(offsetAt(y));
	}
	else if (tab != NONE)
	{
		fscommand("onTab", String(tab));
	}
	else if (posture != NONE)
	{
		fscommand("onPosture", String(posture));
	}
	else if (filterShown && inside(x, y, FILTER_X, SEARCH_Y, FILTER_WIDTH, SEARCH_HEIGHT))
	{
		fscommand("onFilter");
	}
	else if (clearShown && inside(x, y, CLEAR_X, SEARCH_Y, CLEAR_WIDTH, SEARCH_HEIGHT))
	{
		fscommand("onClear");
	}
	else if (hovered != NONE)
	{
		fscommand("onPick", String(hovered));
	}
	else if (inside(x, y, SAVE_X, BUTTON_Y, BUTTON_WIDTH, BUTTON_HEIGHT))
	{
		fscommand("onSave");
	}
	else if (inside(x, y, CANCEL_X, BUTTON_Y, BUTTON_WIDTH, BUTTON_HEIGHT))
	{
		fscommand("onCancel");
	}
};
pointer.onMouseUp = function():Void
{
	dragging = false;
	refocus();
};
pointer.onMouseWheel = function(delta:Number):Void
{
	fscommand("onScroll", String(delta));
};
Mouse.addListener(pointer);

var keys:Object = new Object();
keys.onKeyDown = function():Void
{
	if (Key.getCode() == KEY_ESCAPE)
	{
		fscommand("onCancel");
	}
};
Key.addListener(keys);
