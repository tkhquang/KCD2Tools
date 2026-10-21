KINGDOM COME: DELIVERANCE II - SOUND FRAMEWORK
Custom Sounds and Voices with Lip Sync for Mods
Version 1.1.0

Sound Framework lets other mods add sounds and voice lines to the game, replace
the game's own sounds and voice lines, and give the speakers lip movement and a
facial mood. On its own it changes nothing you can hear: install it for the mods
that need it.

It comes in two builds; install ONE of them:
  - The ASI build (KCD2_SoundFramework_vX.Y.Z.zip) needs an ASI loader.
  - The KCSE build (KCD2_SoundFramework_KCSE_vX.Y.Z.zip) needs Kingdom Come
    Script Extender (KCSE) and installs into the Mods folder.

INSTALLATION - ASI BUILD

Find the installation folder:
  Open your KC:D 2 installation folder, then the Bin/Win64MasterMaster... subfolder
  that contains WHGame.dll. On Steam:
    <KC:D 2 installation folder>/Bin/Win64MasterMasterSteamPGO/
  Other stores use their own Bin/Win64MasterMaster... folder. Use WHGame.dll to
  identify the correct one. The loader and mod files go directly into this folder.

Step 1 - Install an ASI loader (one time):
  If you already have a working ASI loader, skip to Step 2. The same loader can
  load Sound Framework, TPVCamera, Henry's Senses and other ASI mods.
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
  If you use KCSE, it already loads .asi files beside its dinput8.dll, so you
  need no separate loader; you can also use the KCSE build instead.

Step 2 - Install the mod:
  Extract this archive directly into that SAME binary folder, next to WHGame.dll
  and the loader DLL, without an extra archive-named subfolder.

FOLDER LAYOUT (ASI build, Steam):
  Bin/Win64MasterMasterSteamPGO/
  |-- WHGame.dll                             (game file, already present)
  |-- dinput8.dll                            (ASI loader)
  |-- KCD2_SoundFramework.asi                (this mod)
  |-- KCD2_SoundFramework.ini                (settings)
  |-- KCD2_SoundFramework_moods.ini          (default facial moods)
  |-- KCD2_SoundFramework_lipmodel.bin       (advanced lip sync model)
  \-- KCD2_SoundFramework.log                (created when the mod loads)

INSTALLATION - KCSE BUILD

  Install Kingdom Come Script Extender first:
    https://www.nexusmods.com/kingdomcomedeliverance2/mods/3332
  Extract this archive into the game's Mods folder (create it if it is missing):
    <KC:D 2 installation folder>/Mods/
  You get Mods/soundframework/KCSE/Plugins/ with the DLL, the INIs and the model
  file side by side. Mod managers can install the archive as a regular mod.

Install only one build. If both are installed, the second one to load stays off
and the log says so.

VERIFY IT IS WORKING:
  After loading a save, look for KCD2_SoundFramework.log beside the mod's DLL or
  ASI. It lists every sound pack the framework found and any problem in them.
  Mods that use the framework also list their sounds there.

SETTINGS (KCD2_SoundFramework.ini):
  Save the file to apply changes while playing. [LipSync] Enabled needs a restart.
  [LipSync] Mode = ADVANCED moves the whole lower face with a small machine-learned
  model; BASIC follows the loudness of the voice. OffsetMs moves the mouth earlier
  or later than the sound.
  [LipSync] DubbedVoices = AUTO: with a voice dub that has no mouth animation of
  its own, the lips follow each line's own voice instead of the English
  animation, and its smiles and frowns stay. ON always does this, OFF never.
  [Debug] EventLog and SpeakLog help mod authors find the game's sound events and
  voice lines.

QUICK TEST:
  No pack needed. Load a save, open the console (~) and make Henry ("Dude")
  say another character's line in a mood, for example:
    sfw_say Dude Localization/dialog/open_world/lazne_a_lazebnice/aals_obje_lazebnice_ale_ty_uz__wAcW mood:surprised You're already undressed?
    sfw_say Dude Localization/dialog/trosecko/konecZabomysichValek/jmil_comb_bojovnici_ty_odporny_RkVq mood:angry strength:2 You disgusting, slimy, rotten piece of shit!
  These lines have no lip-sync animation in the game, so the mouth movement is
  the framework's own. strength scales the mood from 0 to 2 (default 1); the
  text at the end is the subtitle. A third-person camera mod shows his face.
  More lines are on the mod page and in the README.

UPDATING:
  Back up your customized INI before extracting the new archive, then reapply
  your settings to the supplied INI. KCD2_SoundFramework_moods.ini is replaced on
  update; change moods in your own pack instead (see the modding guide).

LINUX / STEAM DECK (WINE/PROTON):
  With the ASI build, add an override for your loader in Steam's
  Properties -> Launch Options:
    WINEDLLOVERRIDES="dinput8=n,b" %command%
  Use version=n,b or winmm=n,b instead when using that loader name.

MAKING SOUND PACKS:
  See the modding guide:
  https://github.com/tkhquang/KCD2Tools/blob/main/SoundFramework/docs/MODDING.md

LIP SYNC MODEL AND LICENSE:
  Advanced lip sync is machine-learned: a small neural network, trained on the
  game's own voice lines and facial animation, turns each voice line into mouth
  movement while you play. It generates facial animation only. Set
  [LipSync] Mode = BASIC to use the mouth without machine learning.
  The code is MIT licensed. KCD2_SoundFramework_lipmodel.bin is derived from
  Kingdom Come: Deliverance II game data owned by Warhorse Studios and PLAION.
  The MIT license does not cover it: use it only with KC:D 2 through this mod,
  and not in other games, other products or commercially.

Full details and support:
https://github.com/tkhquang/KCD2Tools
