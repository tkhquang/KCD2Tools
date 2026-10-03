KINGDOM COME: DELIVERANCE II - HENRY'S SENSES
Loot and Object Highlighting
Version 0.1.0

This mod is an .asi plugin, so it needs an ASI loader to run. The loader is NOT
included in this download - you install it once, yourself (see Step 1). If you
already have an ASI loader for KC:D 2 from another mod, skip Step 1.

INSTALLATION

Step 1 - Install an ASI loader (one time):
  Download Ultimate ASI Loader by ThirteenAG and place ONE of these DLLs in your
  game's binary folder (the Bin/Win64MasterMaster... folder that contains WHGame.dll):
    - dinput8.dll  (recommended)
    - version.dll  (alternative, if dinput8.dll does not work)
    - winmm.dll    (another alternative)
  All loader variants: https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases

Step 2 - Install the mod:
  Extract KCD2_HenrySenses.asi and KCD2_HenrySenses.ini into that SAME binary folder,
  next to WHGame.dll and the loader DLL. On Steam:
    <KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/
  Custom HenrySenses particle effects also require KCD2_HenrySenses.particles.xml
  beside the INI and an Effect setting in a group. No group uses particles by default.
  Nothing goes into the Mods folder: the loot detection is built into the .asi.

Step 3 - Launch and play:
  Highlight loot and butcherable carcasses: H, or hold RB + press Start.
  Highlight herbs: Left Shift + H, or hold RB + press Back.
  Highlights pulse for 5 seconds, then fade over 1 second, within 20 metres.
  Loot, Butcher and Herbs start enabled; all other groups start disabled.
  The Loot group excludes empty and stolen targets. Butcher separately highlights
  butcherable carcasses. Highlights hide during dialogue by default.
  Group hotkeys also work while menus are open.
  Key.Consume is enabled for both bindings, so the controller's last button is
  hidden from the game while the combination is held. Keyboard keys still reach it.

VERIFY THE LOADER IS WORKING:
  After launching once, look for KCD2_HenrySenses.log in the binary folder. If it is
  not there, the loader is not loading the mod - try one of the other loader DLLs.

Edit KCD2_HenrySenses.ini and save to apply changes while playing. The INI includes
hotkeys, group options, defaults and colour examples. To pick a colour:
  https://www.fleevio.com/en/tools/color-picker
Copy "HEX (no #)" or "HEX + alpha (no #)" into Color or after a target's colon.
Highlights accept RRGGBB or RRGGBBAA; alpha is ignored. FillOpacity controls fill
transparency. FocusTint uses six digits only, without #.

Full details and support:
https://github.com/tkhquang/KCD2Tools
