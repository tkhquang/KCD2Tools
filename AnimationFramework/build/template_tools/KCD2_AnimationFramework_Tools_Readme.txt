KINGDOM COME: DELIVERANCE II - ANIMATION FRAMEWORK TOOLS

Tools for making animation mods. Players do not need them: the framework itself
is the main download. Extract this folder anywhere.

  animfw.exe          The command line. Checks clips, finds the game's
                      fragments and clips, makes and packs mods, compiles
                      Blender exports (.i_caf) without rc.exe and retargets BVH
                      and FBX animations onto Henry. "animfw mcp" serves the
                      same to AI assistants. Run it without arguments for its
                      usage.
  animfw-studio.exe   AnimFW Studio. The same in a window, with a mannequin
                      that plays the clip.

Both read the game from its paks. They find the Steam install by themselves.
For another install, pass --game <folder> to animfw, or set the Game folder in
AnimFW Studio.

MODDING GUIDE
  How to make animation mods, with every command and the rules a clip follows:
  https://github.com/tkhquang/KCD2Tools/blob/main/AnimationFramework/docs/MODDING.md

SOURCE AND ISSUES
  https://github.com/tkhquang/KCD2Tools/tree/main/AnimationFramework
