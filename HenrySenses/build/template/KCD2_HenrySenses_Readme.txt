KINGDOM COME: DELIVERANCE II - HENRY'S SENSES
Loot and Object Highlighting
Version 1.0.0

This mod is an .asi plugin, so it needs an ASI loader to run. The loader is NOT
included in this download - you install it once, yourself (see Step 1). If you
already have an ASI loader for KC:D 2 from another mod, skip Step 1.

INSTALLATION

Find the installation folder:
  Open your KC:D 2 installation folder, then the Bin/Win64MasterMaster... subfolder
  that contains WHGame.dll. On Steam:
    <KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/
  Other stores use their own Bin/Win64MasterMaster... folder. Use WHGame.dll to
  identify the correct one. The loader and mod files go directly into this folder.

Step 1 - Install an ASI loader (one time):
  If you already have a working ASI loader, skip to Step 2. The same loader can
  load Henry's Senses, TPVCamera and other ASI mods.
  Download ONE x64 variant of Ultimate ASI Loader by ThirteenAG:
    - dinput8.dll  (usual choice if this name is unused)
        https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/dinput8-x64.zip
    - version.dll  (alternative when dinput8.dll is used, including by KCSE)
        https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/version-x64.zip
    - winmm.dll    (alternative if version.dll is used or does not load)
        https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/x64-latest/winmm-x64.zip
  Extract the DLL from the chosen ZIP directly beside WHGame.dll. Use x64, not
  Win32, and install only one Ultimate ASI Loader variant. Do not overwrite a
  DLL belonging to another mod.
  All loader variants: https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases

  USING KINGDOM COME SCRIPT EXTENDER (KCSE)?
  KCSE uses dinput8.dll. Keep KCSE's file and install Ultimate ASI Loader as
  version.dll or winmm.dll in the same folder. Choose an unused name from the
  downloads above; do not rename or replace KCSE's DLL.
  KCSE: https://www.nexusmods.com/kingdomcomedeliverance2/mods/3332

Step 2 - Install the mod:
  Extract this archive directly into that SAME binary folder, next to WHGame.dll
  and the loader DLL, without an extra archive-named subfolder.
  KCD2_HenrySenses.asi is the mod; KCD2_HenrySenses.ini contains the settings.
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
  After launching and loading a save, look for KCD2_HenrySenses.log beside the ASI.
  If it is missing, check that the ASI and x64 loader DLL are directly beside
  WHGame.dll, then try another unused loader variant and relaunch. Replace only
  the Ultimate ASI Loader DLL you installed; keep KCSE and other mods' DLLs.
  On Wine/Proton, also check the override below.

FOLDER LAYOUT (Steam):
  Bin/Win64MasterMasterSteamPGO/
  |-- WHGame.dll                         (game file, already present)
  |-- dinput8.dll                        (ASI loader; see KCSE note above)
  |-- KCD2_HenrySenses.asi                (this mod)
  |-- KCD2_HenrySenses.ini                (settings)
  |-- KCD2_HenrySenses.particles.xml      (optional particle effects)
  \-- KCD2_HenrySenses.log                (created when the mod loads)
  With KCSE, dinput8.dll belongs to KCSE; your ASI loader, version.dll or
  winmm.dll, sits alongside it.

UPDATING:
  Back up your customized INI before extracting the new archive, then reapply
  your settings to the supplied INI.

LINUX / STEAM DECK (WINE/PROTON):
  In Steam's Properties -> Launch Options, add an override for your loader:
    WINEDLLOVERRIDES="dinput8=n,b" %command%
  Use version=n,b or winmm=n,b instead when using that loader name. With KCSE's
  dinput8.dll and the version.dll loader, retain both overrides:
    WINEDLLOVERRIDES="dinput8,version=n,b" %command%
  For a command-line launch, set the same WINEDLLOVERRIDES value before your
  launch command. Keep any overrides your other mods need.

Edit KCD2_HenrySenses.ini and save to apply changes while playing. The INI includes
hotkeys, group options, defaults and colour examples. To pick a colour:
  https://www.fleevio.com/en/tools/color-picker
Copy "HEX (no #)" or "HEX + alpha (no #)" into Color or after a target's colon.
Highlights accept RRGGBB or RRGGBBAA; alpha is ignored. FillOpacity controls fill
transparency. FocusTint uses six digits only, without #.

Full details and support:
https://github.com/tkhquang/KCD2Tools
