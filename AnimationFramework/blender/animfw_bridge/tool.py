"""Finds the game and runs animfw.exe, the Animation Framework's command line (compile, retarget, make, pack,
fragments)."""
import os
import re
import subprocess

BIN = os.path.join("Bin", "Win64MasterMasterSteamPGO")
TOOL_NAME = "animfw.exe"
DEV_FOLDER = "KCD2_AnimationFramework_Dev"
LOG_NAME = "KCD2_AnimationFramework.log"
GAME_FOLDER = os.path.join("steamapps", "common", "KingdomComeDeliverance2")


def steam_game_folder():
    """The game's folder in any Steam library, or ''."""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as key:
            steam = winreg.QueryValueEx(key, "SteamPath")[0]
    except OSError:
        return ""
    libraries = [steam]
    try:
        with open(os.path.join(steam, "steamapps", "libraryfolders.vdf"), encoding="utf-8") as vdf:
            libraries += [p.replace("\\\\", "\\") for p in re.findall(r'"path"\s+"([^"]+)"', vdf.read())]
    except OSError:
        pass
    for library in libraries:
        folder = os.path.join(library, GAME_FOLDER)
        if os.path.isdir(os.path.join(folder, "Data")):
            return folder
    return ""


def game_folder(preferences):
    return bpy_abspath(preferences.game_folder) if preferences.game_folder else steam_game_folder()


def bpy_abspath(path):
    import bpy
    return os.path.normpath(bpy.path.abspath(path))


def tool_path(preferences):
    """The tool: the preference, else the one the add-on ships with, else one next to the framework in the game."""
    if preferences.tool_path:
        return bpy_abspath(preferences.tool_path)
    bundled = os.path.join(os.path.dirname(os.path.abspath(__file__)), TOOL_NAME)
    if os.path.isfile(bundled):
        return bundled
    game = game_folder(preferences)
    return os.path.join(game, BIN, TOOL_NAME) if game else ""


def dev_folder(preferences):
    game = game_folder(preferences)
    return os.path.join(game, BIN, DEV_FOLDER) if game else ""


def framework_log(preferences):
    game = game_folder(preferences)
    return os.path.join(game, BIN, LOG_NAME) if game else ""


def mod_id_of(folder):
    """The mod id the tool reads for @p folder: mod.manifest's <modid>, else the folder's name."""
    try:
        with open(os.path.join(folder, "mod.manifest"), encoding="utf-8", errors="replace") as manifest:
            found = re.search(r"<modid>\s*([^<\s]+)\s*</modid>", manifest.read())
            if found:
                return found.group(1)
    except OSError:
        pass
    return os.path.basename(os.path.normpath(folder))


def run(preferences, *arguments, timeout=180):
    """Runs the tool; returns (exit code, output lines). Exit 0 is success, 2 success with warnings."""
    tool = tool_path(preferences)
    if not tool or not os.path.isfile(tool):
        return 1, [f"{TOOL_NAME} was not found: set its path in the add-on's preferences"]
    command = [tool]
    game = game_folder(preferences)
    if game:
        command += ["--game", game]
    command += [str(a) for a in arguments]
    try:
        # Without an explicit encoding the locale's code page decodes, and it raises on bytes it leaves undefined.
        done = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                              timeout=timeout, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except (OSError, subprocess.TimeoutExpired) as error:
        return 1, [str(error)]
    return done.returncode, (done.stdout + done.stderr).splitlines()
