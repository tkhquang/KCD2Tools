# AnimationFramework: the "AnimFW export" toolbar button (Editor/DynamicToolbars/AnimationFramework.xml).
# Installed by `animfw sandbox-setup`, which overwrites this file when it runs again (edit animfw_export.cfg instead).
#
# Copyright (c) 2026-present Quang Trinh (tkhquang) <khacquang.trinh@gmail.com>
# https://github.com/tkhquang/KCD2Tools/tree/main/AnimationFramework
# MIT License: https://github.com/tkhquang/KCD2Tools/blob/main/AnimationFramework/LICENSE
#
# Saves the modified layers, then turns the scene's layer into a scene of your mod with AnimationFramework's tool
# (`animfw sandbox-layer`): a level layer and a cutscene table row in the mod's Data folder, nothing else of the level.
# The scene's layer is the layer of the selected object, else of the Track View sequence's object, else the layer
# selected in the layer list. Settings live in animfw_export.cfg (UTF-8) beside this script (asked for on first use):
#   mod_folder  your mod's source folder (the one holding mod.manifest)
#   animfw      the path of animfw.exe (sandbox-setup writes it)
#   install     optional: where `animfw pack` installs the mod after each export (MO2: mods\<Mod>\<modid>)
#   name        optional: the scene's name (the layer's name by default)
#
# The editor runs Python 2.7, whose strings are bytes and whose subprocess passes the command line through the ANSI
# code page. Paths are therefore text inside this script, and animfw starts through CreateProcessW, so a folder name
# in any language (Chinese on a Western system, for one) reaches it whole.
import ctypes
import io
import os
import subprocess
import sys
from ctypes import wintypes

import general

try:
    import warhorse
except ImportError:
    warhorse = None
try:
    import trackview
except ImportError:
    trackview = None

GUID_LENGTH = 36
KEYS = ("mod_folder", "animfw", "install", "name")
TEXT = type(u"")
PY2 = sys.version_info[0] == 2
# The code a process exits with when it crashes on an access violation (0xC0000005).
ACCESS_VIOLATION = 0xC0000005
STARTF_USESHOWWINDOW = 0x1
STARTF_USESTDHANDLES = 0x100
CREATE_NO_WINDOW = 0x08000000
HANDLE_FLAG_INHERIT = 0x1
INFINITE = 0xFFFFFFFF

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


class STARTUPINFO(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR), ("lpDesktop", wintypes.LPWSTR),
                ("lpTitle", wintypes.LPWSTR), ("dwX", wintypes.DWORD), ("dwY", wintypes.DWORD),
                ("dwXSize", wintypes.DWORD), ("dwYSize", wintypes.DWORD), ("dwXCountChars", wintypes.DWORD),
                ("dwYCountChars", wintypes.DWORD), ("dwFillAttribute", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD), ("lpReserved2", ctypes.c_void_p),
                ("hStdInput", wintypes.HANDLE), ("hStdOutput", wintypes.HANDLE), ("hStdError", wintypes.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE), ("dwProcessId", wintypes.DWORD),
                ("dwThreadId", wintypes.DWORD)]


class SECURITY_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("nLength", wintypes.DWORD), ("lpSecurityDescriptor", ctypes.c_void_p),
                ("bInheritHandle", wintypes.BOOL)]


kernel32.CreateProcessW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p, ctypes.c_void_p,
                                    wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR,
                                    ctypes.POINTER(STARTUPINFO), ctypes.POINTER(PROCESS_INFORMATION)]
kernel32.CreateProcessW.restype = wintypes.BOOL
kernel32.CreatePipe.argtypes = [ctypes.POINTER(wintypes.HANDLE), ctypes.POINTER(wintypes.HANDLE),
                                ctypes.POINTER(SECURITY_ATTRIBUTES), wintypes.DWORD]
kernel32.CreatePipe.restype = wintypes.BOOL
kernel32.SetHandleInformation.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD]
kernel32.SetHandleInformation.restype = wintypes.BOOL
kernel32.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                              ctypes.c_void_p]
kernel32.ReadFile.restype = wintypes.BOOL
kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel32.WaitForSingleObject.restype = wintypes.DWORD
kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
kernel32.GetExitCodeProcess.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL


def as_text(value):
    """Text from an editor or file system string, which Python 2 gives as bytes: UTF-8, else the ANSI code page."""
    if isinstance(value, TEXT):
        return value
    try:
        return value.decode("utf-8")
    except UnicodeDecodeError:
        return value.decode("mbcs")


def to_editor(text):
    """What the editor's functions take: UTF-8 bytes in Python 2, text in Python 3."""
    return text.encode("utf-8") if PY2 else text


def message(text):
    general.message_box_ok(to_editor(text))


def log(text):
    for line in text.splitlines():
        general.log(to_editor(u"[AnimFW export] " + line))


def game_root():
    data = as_text(general.get_game_folder())
    return data[: -len(u"\\data")] if data.lower().endswith(u"\\data") else os.path.dirname(data)


def script_folder():
    try:
        return os.path.dirname(os.path.abspath(as_text(__file__)))
    except NameError:
        return os.path.join(game_root(), u"Editor", u"Scripts")


CONFIG = os.path.join(script_folder(), u"animfw_export.cfg")


def read_config():
    values = {}
    if os.path.isfile(CONFIG):
        with open(CONFIG, "rb") as config:
            text = as_text(config.read()).lstrip(u"\ufeff")
        for line in text.splitlines():
            line = line.strip()
            if line and not line.startswith(u"#") and u"=" in line:
                key, value = line.split(u"=", 1)
                values[key.strip()] = value.strip().strip(u'"')
    return values


def write_config(values):
    lines = [u"# AnimationFramework export settings (see animfw_export.py)"]
    lines += [u"%s=%s" % (key, values.get(key, u"")) for key in KEYS]
    with io.open(CONFIG, "w", encoding="utf-8", newline=u"\n") as config:
        config.write(u"\n".join(lines) + u"\n")


def ask(values, key, question):
    if values.get(key):
        return True
    answer = general.edit_box(to_editor(question))
    if not answer:
        return False
    values[key] = as_text(answer).strip().strip(u'"')
    write_config(values)
    return True


def layer_name():
    """The full name (<name>_<guid>) of the scene's layer."""
    names = general.get_names_of_selected_objects()
    if names:
        return as_text(general.get_object_layer([names[0]])[0])
    if trackview is not None:
        sequence = trackview.get_current_sequence()
        if sequence:
            try:
                return as_text(general.get_object_layer([sequence])[0])
            except Exception:
                pass
    try:
        return as_text(general.get_name_of_selected_layer())
    except Exception:
        return u""


def layer_path(full_name):
    """The .lyr path of a layer, from the layer tree (as the editor's own scripts build it), else by its name."""
    level = os.path.join(game_root(), as_text(general.get_current_level_path()), u"Layers")
    # Only a name that ends with _<guid> carries one: a plain name longer than a GUID keeps its last characters.
    has_guid = len(full_name) > GUID_LENGTH and full_name[-GUID_LENGTH - 1] == u"_"
    guid = full_name[-GUID_LENGTH:] if has_guid else u""
    if warhorse is not None and guid:
        try:
            parents = {}
            main = as_text(warhorse.get_main_layer_guid())
            open_guids, seen = [main], set()
            while open_guids and len(seen) < 20000:
                current = open_guids.pop()
                if current in seen:
                    continue
                seen.add(current)
                if current == guid:
                    break
                for child in warhorse.get_child_layers(to_editor(current)):
                    child = as_text(child)
                    parents[child] = current
                    open_guids.append(child)
            parts, current = [as_text(warhorse.get_layer_short_name_by_guid(to_editor(guid))) + u".lyr"], guid
            while current != main and current in parents:
                current = parents[current]
                if current != main:
                    parts.insert(0, as_text(warhorse.get_layer_short_name_by_guid(to_editor(current))))
            candidate = os.path.join(level, *parts)
            if os.path.isfile(candidate):
                return candidate
        except Exception:
            pass
    short = full_name[: -GUID_LENGTH - 1] if guid else full_name
    short = short.replace(u"\\", u"/").split(u"/")[-1].lower() + u".lyr"
    for folder, _, files in os.walk(level):
        for name in files:
            if name.lower() == short:
                return os.path.join(folder, name)
    return u""


def start(arguments, output=None):
    """Starts a program with no window through CreateProcessW and returns its process handle. With output (an
    inheritable pipe end), the program's stdout and stderr go there."""
    info = STARTUPINFO()
    info.cb = ctypes.sizeof(STARTUPINFO)
    info.dwFlags = STARTF_USESHOWWINDOW  # with wShowWindow 0, SW_HIDE
    if output is not None:
        info.dwFlags |= STARTF_USESTDHANDLES
        info.hStdOutput = output
        info.hStdError = output
    process = PROCESS_INFORMATION()
    command = ctypes.create_unicode_buffer(subprocess.list2cmdline([as_text(a) for a in arguments]))
    if not kernel32.CreateProcessW(None, command, None, None, output is not None, CREATE_NO_WINDOW, None, None,
                                   ctypes.byref(info), ctypes.byref(process)):
        raise ctypes.WinError(ctypes.get_last_error())
    kernel32.CloseHandle(process.hThread)
    return process.hProcess


def run(arguments):
    """Runs animfw without a console window and returns (exit code, output)."""
    read_end, write_end = wintypes.HANDLE(), wintypes.HANDLE()
    security = SECURITY_ATTRIBUTES(ctypes.sizeof(SECURITY_ATTRIBUTES), None, True)
    if not kernel32.CreatePipe(ctypes.byref(read_end), ctypes.byref(write_end), ctypes.byref(security), 0):
        return -1, u"could not make a pipe to run %s" % as_text(arguments[0])
    # Only the program's end is inherited, or the pipe never reports its end.
    kernel32.SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0)
    try:
        process = start(arguments, write_end)
    except OSError as error:
        kernel32.CloseHandle(read_end)
        kernel32.CloseHandle(write_end)
        return -1, u"could not run %s: %s" % (as_text(arguments[0]), as_text(str(error)))
    kernel32.CloseHandle(write_end)
    chunks = []
    chunk = ctypes.create_string_buffer(4096)
    count = wintypes.DWORD()
    while kernel32.ReadFile(read_end, chunk, len(chunk), ctypes.byref(count), None) and count.value:
        chunks.append(chunk.raw[: count.value])
    kernel32.CloseHandle(read_end)
    kernel32.WaitForSingleObject(process, INFINITE)
    code = wintypes.DWORD()
    kernel32.GetExitCodeProcess(process, ctypes.byref(code))
    kernel32.CloseHandle(process)
    output = b"".join(chunks).decode("utf-8", "replace")
    command = subprocess.list2cmdline([as_text(a) for a in arguments])
    program = os.path.basename(as_text(arguments[0]))
    if code.value == ACCESS_VIOLATION and not output.strip():
        # A crash before the program printed anything: with the editor's skin on (Codejock "Skinning"), every
        # program the editor starts crashes like this, whatever the program.
        output = (u"The editor could not start %s.\n\n"
                  u"Why: the editor's Dark/Light Skin works by hooking Windows inside the editor, and while a skin\n"
                  u"is on, every program the editor starts crashes at once (any program, not only animfw).\n\n"
                  u"Fix: View > No Skinning, then click AnimFW export again (no restart needed).\n\n"
                  u"Or keep the skin and run the same export yourself:\n%s" % (program, command))
    elif code.value != 0 and not output.strip():
        output = u"%s exited with code %d and no output\n(command: %s)" % (program, code.value, command)
    return code.value, output


def close_no_changes_note(animfw):
    """save_modified_layers() shows an OK-only "No modified layers found" note when nothing changed. animfw closes it
    (that note and nothing else), so the button needs one click."""
    try:
        kernel32.CloseHandle(start([animfw, u"sandbox-note", TEXT(os.getpid())]))
    except OSError:
        pass


def export():
    values = read_config()
    if not ask(values, "mod_folder", u"AnimFW export: your mod's source folder (the one holding mod.manifest)"):
        return
    if not ask(values, "animfw", u"AnimFW export: the path of animfw.exe"):
        return
    if not os.path.isfile(values["animfw"]):
        message(u"AnimFW export: animfw.exe was not found at\n%s\n\nFix it in %s" % (values["animfw"], CONFIG))
        return

    full_name = layer_name()
    if not full_name:
        message(u"AnimFW export: select an object of the scene (or its layer) first.")
        return
    close_no_changes_note(values["animfw"])
    general.save_modified_layers()
    path = layer_path(full_name)
    if not path:
        message(u"AnimFW export: no .lyr file was found for the layer\n%s" % full_name)
        return

    arguments = [values["animfw"], u"sandbox-layer", values["mod_folder"], path]
    if values.get("name"):
        arguments += [u"--as", values["name"]]
    code, output = run(arguments)
    log(output)
    report = output.strip().replace(values["mod_folder"], u"<mod>")
    if code == 0 and values.get("install"):
        packed, pack_output = run([values["animfw"], u"pack", values["mod_folder"], values["install"]])
        log(pack_output)
        report += u"\n\n" + (pack_output.strip() if packed == 0
                             else u"pack failed (is the game running?):\n" + pack_output.strip())
    title = u"AnimFW export: done" if code == 0 else u"AnimFW export: failed"
    message(u"%s\n\n%s\n\n(full output in the console)" % (title, report))


export()
