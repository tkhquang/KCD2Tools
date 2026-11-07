# Gesture Wheel

A wheel of gestures for Henry. Hold G, point at a gesture and let go: Henry plays it, says a line, and the person in
front of him reacts.

> This mod is a showcase of what the [Animation Framework](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3981)
> can do. It is made for posing, screenshots and third-person play, and adds little to the gameplay itself.

- **Eight gestures ready to use:** wave, sword pose, play the flute, sit on the ground, read, laugh, rude and threaten.
- **Henry speaks:** each gesture plays one of Henry's own voice lines.
- **People react:** they nod or refuse, answer, like Henry more or less, and may attack after repeated threats.
- **Make your own:** change any slot in game, with any animation, voice line, reaction and item from the game.
- **Seated and on horseback:** Henry plays the upper body of the gesture and stays in his seat or saddle.
- Open source, built on the [Animation Framework](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3981) (built with [DetourModKit](https://github.com/tkhquang/DetourModKit)).

## Requirements

- [Animation Framework](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3981) 1.0.0 or newer.
- Optional: [Sound Framework](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3860) 1.3.0 or newer, to search the game's voice lines in the editor and to give people exact lines to say.
- Optional: PTF extender, so the wheel also opens while a person's chat prompt is on screen.

## Installation

Install with your mod manager, or extract into `<game>/Mods/`. To uninstall, delete `Mods/gesture_wheel`.

Your own gestures are saved in `Mods/gesture_wheel/gesture_wheel.settings` (with Mod Organizer 2, in its `overwrite`
folder).

## Controls

| | Keyboard and mouse | Controller |
|---|---|---|
| Open the wheel | Hold G | Hold RB, press Y |
| Point at a gesture | Mouse | Right stick |
| Play it | Let go of G, or click | Let go of RB |
| Close without playing | Let go in the middle, or right click | B |
| Edit the slot | E | - |

**Ending a gesture:** W, A, S, D, Space, the left stick or B end it at once, so a pose such as sitting on the ground
lasts until you move. Only these default keys work, not rebound ones. You can open the inventory during a gesture.

**Torch:** when a gesture has no version for a torch, the torch is hidden while it plays. Its light stays on.

## Editing a slot

Hold G, point at a slot and press E. The editor shows the slot's choices in gold. Click a row to add or remove it,
then Save. Esc or right click closes it without saving.

- **Animation:** any animation in the game. With several, Henry picks one at random. The buttons on top choose what
  plays standing, on a seat or on horseback.
- **Sequence:** a start, a loop and an end that play in order, for poses that last. Choose whether the pose holds and
  when it ends.
- **Voice line:** any voice line in the game, found by its words, speaker or actor. Click a line to add it and hear it.
- **NPC reaction:** how the person in front of Henry (within 6 m) reacts: their animation, what they say, how much they
  like Henry, and when they attack. Use a built-in reaction or press New to make your own.
- **Name & options:**
  - The name on the wheel.
  - *No collision* (on by default): Henry can touch people without the gesture stopping, but may pass through walls.
  - *Locked in*: moving does not end the gesture, and a looping one stops after 15 seconds.
  - *Hands*: draw a weapon or a torch for the gesture, or put them away. All is given back after.
  - *Right hand*, *Left hand*, *In front*: hold any item from the game while the gesture plays, such as a fishing rod,
    a mug or a book.
  - *Use*: put another gesture on this slot.

Nothing stops odd choices: an animation made for a chair sits Henry on the air.

## For mod authors

Add a gesture from your own mod's Lua script. Mods load in any order, so add it to the queue. The wheel picks it up
when the game starts:

```lua
GestureWheel = GestureWheel or {}
GestureWheel.Pending = GestureWheel.Pending or {}
table.insert(GestureWheel.Pending, {
    id = "my_mod_cheer",
    label = "Cheer",
    fragment = "ADLG_Agree",
    voice = "kutnohorsko/damaVNesnazich/tmck_kont_henry_dobre_4F2Y",
    reaction = "friendly",
})
```

Once the wheel has loaded, `GestureWheel.Register{...}` takes the same table.

| Field | Meaning |
|---|---|
| `id`, `label` | Required. A unique name, and the text on the wheel. The same `id` replaces a gesture. |
| `fragment` | What Henry plays standing: a Mannequin fragment, or a list to pick from at random. |
| `seated`, `mounted` | The same on a seat and on a horse. Without them Henry plays the seat or horse version of `fragment`, else its upper body. |
| `intro`, `loop`, `outro` | Lists like `fragment` that play in order. A gesture needs at least one animation list. |
| `hold`, `duration` | `true` keeps the last pose; `duration` ends it after that many seconds. |
| `voice` | A voice file under `Localization/dialog/` without its extension, or a list. |
| `reaction` | `friendly`, `annoyed`, `threatened`, `audience`, or the id of your own reaction. |
| `hands` | `"melee"`, `"ranged"`, `"torch"` or `"empty"`. |
| `props` | Items to hold: `{ right = { model = "objects/....cgf", label = "..." }, left = ..., front = ... }`. `model = "-"` leaves that hand empty. |
| `slot` | 1 to 8, clockwise from the top. Without it the gesture shows as a *Use* row in the editor. |
| `passThrough`, `lock` | `passThrough = false` keeps collision on; `lock = true` locks Henry in the gesture. |

A list entry can also be a table: `{ fragment = "...", tags = "angry", partial = true, seat = "", horse = "" }`.
`tags` are the tags the animation needs (joined with `+`), `partial` marks one that moves only the face or upper body,
and `seat` or `horse` hold the tags of its seat or horse version (`""` for none needed).

Reactions work the same way, with `GestureWheel.PendingReactions` or `GestureWheel.RegisterReaction{...}`:

```lua
GestureWheel.PendingReactions = GestureWheel.PendingReactions or {}
table.insert(GestureWheel.PendingReactions, {
    id = "my_mod_scared",
    label = "Scared",
    animations = { { fragment = "ADLG_Disagree", women = true } }, -- women = true: women play it too
    seated = { { fragment = "ADLG_Disagree", women = true } },
    barks = { "NPC_POSILA_HRACE_PRYC" },  -- the person says one of their own lines of this type
    lines = {},                           -- exact voice files, needs Sound Framework
    reputation = "greeting_decrease",     -- a row of the game's reputation_change.xml
    change = -0.02,
    fight = { count = 3, window = 30 },   -- attack at the 3rd gesture, each within 30 s of the last
})
```

## License

The mod's code is MIT licensed. The game text and animation data made from Kingdom Come: Deliverance II are not
covered by it: see [LICENSE](LICENSE).
