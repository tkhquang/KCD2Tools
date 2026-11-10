# Making a Cutscene

The framework plays your own in-game cutscenes through the game's cutscene player, the one its quests use. Letterbox, hidden HUD, fades, skipping, music, voices, subtitles and doubles of Henry all work as in the game's own scenes. Your mod adds new files only, so it works next to every other mod.

You can:

- copy one of the game's cutscenes and change it
- build a new scene in the game's Sandbox editor, or by hand around wherever Henry stands
- play it from your own script, from the console, or from a quest

The [Cutscene Examples](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3996) mod ([source](../../CutsceneExamples/)) does both: a dance battle written from scratch and a game cutscene copied and changed, each started from a dialog topic. Its [devlog](../../CutsceneExamples/docs/DEVLOG.md) walks through them.

## How a game cutscene is built

| Part | What it is |
|---|---|
| **Layer** | A level file, `Levels/<level>/layers/<scene>.xml`, with the cast (actors, cameras, lights, props, sound sources) and a `SequenceObject` that holds the timeline. |
| **Sequence** | The timeline, a Track View sequence: who moves, which camera shows, which voice plays, and when. |
| **Profile** | A switch that loads the layer while the scene plays and unloads it after. |
| **Holder** | A `CutsceneHolder` entity, plus the `IngameCutsceneData` it links to, that names the row, the profile and the sequence. |
| **Row** | A line of the cutscene table, `Libs/Tables/ui/cutscene.xml`: time of day, weather, checkpoints, and copying Henry's looks to his double. |

A quest starts the scene: its `CutsceneHandler` node hands the holder to the cutscene player.

## Editing the level, and why not

Without the framework, a new cutscene goes into the level itself:

1. Build the scene on its own layer in Sandbox and export the level.
2. Add a profile for the layer to the level's profiles.
3. Add the holder and its data entity to the level's mission objects.
4. Add the row to the cutscene table (`cutscene__<modid>.xml`).
5. Write a quest with a `CutsceneHandler` node to start it.

Steps 2 and 3 replace files of the level that every mod changing that level also replaces:

| | Editing the level | With the framework |
|---|---|---|
| Your files | Replace the level's own files | New files only |
| Other mods on that level | The last one installed wins, the others break | All of them work |
| Game updates | Your old copy hides what the update changes in that level | Nothing to redo |
| Entity ids | Unique across the whole level, kept by hand | Moved to free ids each time the layer loads |
| Starting it | A quest, always | One Lua line from a script, the console or a quest |
| Where it plays | Where you built it | There, or around Henry |
| After a change | Export the whole level again | Export one layer |

The framework makes the profile and the holder while the game runs, so your mod ships only the layer, the row and a line of Lua.

## Folder layout

```text
my_mod/                                            (your mod's source folder, named after the mod id)
|-- mod.manifest
|-- Data/
|   |-- Levels/kutnohorsko/layers/my_scene.xml     (the layer)
|   |-- Libs/Tables/ui/cutscene__my_mod.xml        (the row)
|   |-- Scripts/Mods/my_mod.lua                    (registers and plays the scene)
|   \-- Quests/my_mod.xml                          (optional: a quest that starts it)
\-- Localization/English_xml/
    \-- text_ui_dialog__my_mod.xml                 (optional: subtitles)
```

To install it:

```bat
animfw pack D:\Mods\my_mod "D:\MO2\mods\My Mod\my_mod"
```

It writes `Data\my_mod.pak`, `Data\Levels\<level>\my_mod.pak` with the layers, and `Localization\English_xml.pak` with the subtitles.

## Setup

- **The framework**, with `Cutscenes` on in its INI (the default). See the [README](../README.md#installing).
- **`animfw.exe`** from the Tools download, to copy and convert scenes.
- **For Sandbox only:** the game's editor, *Kingdom Come: Deliverance II Mod Tools* on Steam.
  1. Run this once:

     ```bat
     animfw sandbox-setup --mod D:\Mods\my_mod
     ```

     It adds an **AnimFW export** button to the editor and points it at your mod.
  2. Restart the editor if it was open.
  3. In the editor, choose **View > No Skinning**. While a Dark or Light skin is on, every program the editor starts crashes, the button too.

  **Overlapping toolbars at 150% or 200% display scaling:** right-click `Bin\Win64ReleaseSteamLTO_DLL\Editor.exe` in the editor's folder, then **Properties > Compatibility > Change high DPI settings**. Tick **Override high DPI scaling behavior** and choose **System (Enhanced)**.

  Run the setup again after updating the framework. To take the button out:

  ```bat
  animfw sandbox-setup --remove
  ```

## Copy a game cutscene and change it

```bat
animfw clone-cutscene D:\Mods\my_mod cin_s9921k_crime__branding_kutnahora my_branding
```

This writes a copy of the scene's layer and row into your mod under the new name, with new entity GUIDs, and prints the Lua line that registers it. The game's scene stays as it is. `--row <row>` picks the row when the tool cannot find an in-game cutscene row for the scene.

`--at <x>,<y>,<z>` plays the copy somewhere else in the same level: the point its characters stand at lands on that spot (take it from Henry's position on the ground there), and its cameras, lights and props move along. The scene keeps its heading. What belongs to the level stays behind, so pick open ground and remove the people and props that only made sense in the original place.

Then change the copy in `Data\Levels\<level>\layers\my_branding.xml`:

- **Animations:** the `anim` of an actor's Animation keys.
- **Cameras:** the Position, Rotation and FOV keys of a camera node.
- **Voices and music:** the `VoiceFileName` of the keys on a `MusicCutscene` node.
- **Actors:** an `AnimChar`'s body, clothing and weapon presets. `guidBodyPresetId` is a soul: it brings the face and a default outfit, so a character with his own costume needs his `guidClothingPresetId` too (copy both from one of his cutscene doubles). Simulated cloth, such as a long coat, shows only with `esClothingConfig="male"` on the `AnimChar`.
- **Settings:** the row in `cutscene__my_mod.xml` (see [Rules](#rules)).

The copy is a game-format layer, which the editor cannot open. To rework a scene in Sandbox, edit the game's scene there and export it (below).

## Make a new scene

### In Sandbox

1. Open the level and build the scene on its own layer, as the game's cinematic layers are built: the cast, a `SequenceObject` with its Track View sequence, and the cameras, lights and sounds it uses. A copy of a game cinematic is the quickest start.
2. Select any object of the scene and click **AnimFW export**. It saves the layers and writes the layer and the row into your mod. Click it again after each change.

- The layer's name is the scene's name (layer, sequence and row): lowercase letters, digits and underscores.
- A scene made from a game cinematic keeps that cinematic's row when it is an in-game cutscene row. Any other scene gets a plain row to extend.
- Only that layer is exported, not the level. Its entities are numbered 1, 2, 3..., and the framework moves them to free ids each time the layer loads.
- A link to an entity of another layer keeps pointing at the game's own entity. A link to an entity neither the scene nor the game's level has (such as a prefab's port) is left out, and the export says how many.

The button runs this, which also works from a command prompt (`--as <name>` and `--row <row>` are optional):

```bat
animfw sandbox-layer D:\Mods\my_mod <KCD2Mod>\Data\Levels\kutnohorsko\Layers\my_scene.lyr
```

### By hand, around Henry

A scene can play wherever Henry stands. Its cast is spawned when it starts and removed when it ends:

```lua
AnimationFramework.Cutscene.Play({
  name = "my_greeting", row = "my_greeting_row", layer = "my_greeting_stub", sequence = "my_greeting",
  sequenceFile = "sequences/my_greeting.xml", anchor = "player",
  cast = {
    { name = "my_greeting_cam", class = "CameraSource", pos = { 0, 0, 2 } },
    { name = "my_greeting_voice", class = "MusicCutscene", pos = { 0, 0, 1.6 } },
  },
})
```

- `sequenceFile` holds one `<Sequence>`, as a layer's `SequenceObject` holds it. Its nodes find the cast by `Name`.
- Positions in the sequence are relative to `anchor`, forward along +Y. `anchor` is `"player"` (Henry's position and heading) or `{ x, y, z, yaw }`.
- Each `cast` entry has a `name`, a `class`, a `pos` relative to the anchor, an optional `yaw` and the entity's `properties`.
- The layer still needs a file, even an empty one: `<Objects />`.

## Playing a scene

Register the scene once gameplay starts, then play it whenever you like. No quest is needed:

```lua
local MyMod = {}

function MyMod:OnEvent(action, event)
  if action == "System" and event == "OnGameplayStarted"
      and AnimationFramework and AnimationFramework.Cutscene then
    AnimationFramework.Cutscene.Register({
      name = "my_branding", row = "my_branding", layer = "my_branding", sequence = "my_branding",
      onEnd = function(def, played) System.LogAlways("my_branding played: " .. tostring(played)) end,
    })
  end
end

UIAction.RegisterEventSystemListener(MyMod, "", "", "OnEvent")
```

From your script, on any event your mod listens to:

```lua
AnimationFramework.Cutscene.Play("my_branding")
```

From the console, to test:

```text
#AnimationFramework.Cutscene.Play('my_branding')
```

`Play` takes a registered name or the table itself. It does nothing, and logs why, when a field is missing, or while the same scene or another scene on its layer still plays. When the game refuses the scene, `kcd.log` says why and `onEnd` gets `played` false.

| Field | |
|---|---|
| `name` | The scene's name, for `Play("name")`. |
| `row` | The row of the cutscene table. |
| `layer` | The layer file's name, `Levels/<level>/layers/<layer>.xml`. |
| `sequence` | The Track View sequence's name. |
| `onEnd(def, played)` | Runs when the scene ends. `played` is false when the game refused to start it. |
| `onEvent(def, event, param)` | Runs for each key of an Event track in the sequence, as the scene reaches it. |
| `signals = { [event] = itemClass }` | Gives the player one item of that class when the event fires, and takes it back 2 seconds later. |
| `signalItem` | The same, once the scene has played. |
| `speakers = { [voice] = name or { names } }` | With [SoundFramework](../../SoundFramework/docs/MODDING.md#lip-movement-in-your-cutscenes): whose mouths follow a voice line. `voice` is the recording's path, as the Voice key names it. |
| `teleport = { x, y, z }` | Where Henry stands when the scene ends. |
| `soundBank = name or { names }` | The game cutscenes whose sound banks the scene's Sound keys use. The copy and export tools fill it in. |
| `layerId` | The layer's runtime id. By default a number from the layer's name, above every id the game's levels use. |
| `profile` | The profile the scene switches on. By default one named after the layer. |
| `anchor`, `cast`, `sequenceFile` | A scene around Henry (above). |

## Starting a scene from a quest

The game's `ExecuteConsoleString` quest node ships empty, and the framework restores it. A `#` command runs as Lua:

```xml
<Function Name="my_scene_start" MethodName="wh::conceptmodule::ExecuteConsoleString" DeclaringType="wh::conceptmodule">
  <Constant Name="Command" Value="#AnimationFramework.Cutscene.Play('my_branding')" />
  <Edge From="my_trigger.OnAcquire" To="Exec" />
</Function>
```

- The game loads a mod's quests from `Quests/<modid>.xml`, so name the project file after your mod id.
- **To react to the scene,** give it `signals` (an event of the sequence) or `signalItem` (its end), and wait for that item with an `ItemDescriptorTrigger` on the player's inventory.
- Use item classes of your own. A game item can move the game's own quests on.

## Rules

- **Cut Scene flag:** the sequence needs its Cut Scene flag (8), or the game refuses it. The game's scenes also hide the HUD and the player.
- **Checkpoints:** each name in the row's `Checkpoints` needs an `<Event Name="...">` in the sequence's `<Events>`, or the scene is refused.
- **Fades** work on a node of type `ScreenFader` named `ScreenFader`, not on the Director. The game also fades to black before and after every cutscene.
- **Sound** plays only from a `MusicCutscene` node: voices, music (a voice key with `IsMusic`) and Sound track triggers. Keys on an actor's node are silent.
- **Sound effects** made for one game cutscene (its crowds, fire, foley) sit in that cutscene's sound bank, which the game loads only for that cutscene. Copied and exported scenes list theirs in `soundBank` already. In a scene you write, or for a sound you add from another cutscene, list those cutscenes in `soundBank`, or the effects stay silent. In the game's `Libs/GameAudio/cinematics.xml`, an event's `path="specific/<cutscene>"` names its cutscene. The game's cutscene footsteps (`cin_fs_*`) are always loaded.
- **Subtitles:** the key is the voice file's name without folder and extension. Its text goes in `text_ui_dialog__<modid>.xml`.
- **Text on a black screen** ("Some time later..."): hold the `ScreenFader` on black and give the scene's row a `CustomText` event, as the game's own scenes do. `StringData` names a line of `text_ui_dialog__<modid>.xml`. Frames count at 30 per second from the start of the sequence. `Layout` is `Center`, `CenterBig` or `CenterBottom`. `FadeIn` and `FadeOut` are in frames too:

  ```xml
  <IngameCutscene Name="my_scene">
    <Events>
      <Event Type="CustomText" StartFrame="30" EndFrame="180" StringData="my_mod_later" Layout="Center" FadeIn="30" FadeOut="30" />
    </Events>
  </IngameCutscene>
  ```

  ```xml
  <Node Type="ScreenFader" Name="ScreenFader">
    <Track paramType="ScreenFader">
      <Key time="0" fadeTime="0.01" fadeColor="0,0,0" fadeType="1" />
      <Key time="6.5" fadeTime="1" fadeColor="0,0,0" fadeType="0" />
    </Track>
  </Node>
  ```

  `fadeType="1"` fades to the color and `fadeType="0"` back from it, onto the scene's next shot.
- **Lip movement:** the game's mouths are baked into its actors' animations. For your own lines, use SoundFramework (`speakers`).
- **Henry's double:** an `AnimChar` with Henry's body preset (`4c2dcffb-dea1-6263-72d7-b39f4db2d8b5`) wears his current armour, because the row copies looks from the character (`CopySoulVisual`, on by default). A double with no character behind it needs its full body, clothing and weapon presets.
- **Hiding NPCs:** list `SequenceArea` entities in the sequence's `<CleanupAreas>`, and the game hides the people inside them during the scene. Copied and converted scenes keep theirs. Don't bind a live NPC to a node just to hide them: the game takes over every actor a sequence binds, and a seated one walks around in their sitting pose afterwards.
- **Chaining animations:** two keys blend only where they overlap. Start the next clip about a second before a one-shot ends, and keep at most two keys playing at once. A key's `start` and `end` crop its clip.
- **Curve keys:** a key's `ds` and `dd` are its in and out handles, as (seconds, value). Keep each under a third of the gap to its neighbour key, or the curve runs backwards in time and the camera stutters. A one-value track (FOV, RTPC) holds its keys directly; only Position, Rotation and DepthOfField split them into `NewSubTrack`s.
- **Footsteps:** the game's cutscene footsteps (`cin_fs_soil1_fast`, `cin_fs_grassdry`, `cin_fs_grass_soft`, `cin_fs_wood`, ...) come per ground type, so match the ground the scene stands on. They sound the same at any camera distance: for a far shot, use a soft one or none.
- **Row settings:** `Time` sets the time of day for the scene only. `StartWeather`, `EndWeather`, `DisposeOfCorpses` and `StopWorldTime` work as in the game's rows.
- **Skipping:** the player presses Space (or the pad's B) twice.

## Testing

- A change to a layer, a row or a quest needs a game restart, because the game keeps the paks open. A change to your Lua does not.
- `animfw_cutscene_playing` (console variable) lists the holders of the framework's scenes the game is playing.
- `KCD2_AnimationFramework.log` beside `WHGame.dll` logs each scene layer as it loads (`scene layer '<name>' takes N free entity ids`). `kcd.log` says why a scene did not start.
