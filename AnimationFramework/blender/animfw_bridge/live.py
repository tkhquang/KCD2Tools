"""Talks to the running game and waits for the Animation Framework's log to confirm each step.

The framework listens on the local pipe \\\\.\\pipe\\KCD2_AnimationFramework (its src/live_bridge.hpp). One message is
one request, an operation and its fields separated by tabs; the answer is "ok" or "error", a tab and a line of text.
The add-on says hello first and checks the protocol version in the answer. The framework runs each request on the
game's main thread as the same Lua as the runner fallback below.

A framework without the pipe (a version without the bridge, or BlenderBridge off in its INI) is reached through the
KCD2 Lua runner instead (vscodelua.asi, github.com/yobson1/kcd2lua): it listens on 127.0.0.1:28771 and takes a
little-endian u32 length, then comma-separated absolute paths of .lua files and a NUL; it runs them on the game thread
once a world is loaded and answers "Files queued for execution", then "All scripts executed successfully" or
"Error in <file>: <message>".
"""
import os
import socket
import struct
import tempfile
import time
from multiprocessing.connection import Client

PIPE = r"\\.\pipe\KCD2_AnimationFramework"
PROTOCOL = "1"
PORT = 28771
OWN_SCOPES_TAG = "animfw_own_scopes"
# The framework answers within its own 10 s wait for a game frame; this covers that and the run.
PIPE_TIMEOUT = 20.0
NOT_RUNNING = ("the game did not answer: start it with the Animation Framework and load a save "
               "(BlenderBridge = true in KCD2_AnimationFramework.ini; an older framework needs the KCD2 Lua runner)")


def reload_clips():
    """Has the game pick up the DEV folder's new and changed clips; returns (ok, text)."""
    return _send(("reload_clips",), "System.ExecuteCommand('animfw_reload_clips')")


def make_action(mod_id, fragment, clip, like, loop, upper):
    """Adds the action to the mod made in the DEV folder and reloads the databases; returns (ok, text)."""
    words = ["animfw_make_mod", mod_id, fragment, clip, like] + (["loop"] if loop else []) + (["upper"] if upper else [])
    return _send(("make", mod_id, fragment, clip, like, _flag(loop), _flag(upper)),
                 "System.ExecuteCommand(" + lua_string(" ".join(words)) + ")")


def play(fragment, upper):
    """Plays the action on Henry (on its own scopes when @p upper); returns (ok, text)."""
    tags = OWN_SCOPES_TAG if upper else ""
    return _send(("play", fragment, _flag(upper)),
                 f"player.human:PlayAnim({lua_string(fragment)}, {lua_string(tags)})")


def stop():
    """Ends the action on Henry; returns (ok, text)."""
    return _send(("stop",), "player.human:StopAnim()")


def _flag(value):
    return "1" if value else "0"


def _send(fields, runner_lua):
    """Sends @p fields through the framework's pipe, or @p runner_lua through the Lua runner when no pipe exists."""
    reply = _pipe_request(fields)
    if reply is not None:
        return reply
    ok, text = run_lua(runner_lua)
    return (ok, text) if ok or not text.startswith("the game's Lua runner did not answer") else (False, NOT_RUNNING)


def _pipe_request(fields):
    """(ok, text) from the framework's pipe, or None when no framework listens on it."""
    try:
        connection = Client(PIPE)
    except FileNotFoundError:
        return None
    except OSError as error:
        return False, f"the Animation Framework's pipe refused the connection ({error})"
    try:
        with connection:
            ok, text = _exchange(connection, "hello")
            if not ok:
                return False, text
            version = text.split(" ", 1)[0]
            if version != PROTOCOL:
                return False, (f"the Animation Framework speaks bridge protocol {version}, this add-on {PROTOCOL}: "
                               "install the add-on that came with the framework")
            return _exchange(connection, "\t".join(fields))
    except (OSError, EOFError) as error:
        return False, f"the game closed the connection ({error})"


def _exchange(connection, message):
    """One request and its answer on an open pipe connection."""
    connection.send_bytes(message.encode("utf-8"))
    if not connection.poll(PIPE_TIMEOUT):
        return False, f"the Animation Framework did not answer within {PIPE_TIMEOUT:.0f} s"
    status, _, text = connection.recv_bytes().decode("utf-8", "replace").partition("\t")
    return status == "ok", text


def run_lua(code, timeout=15.0):
    """Runs @p code in the game; returns (ok, the runner's reply)."""
    handle, path = tempfile.mkstemp(prefix="animfw_bridge_", suffix=".lua")
    with os.fdopen(handle, "w", encoding="utf-8") as script:
        script.write(code)
    body = path.encode("utf-8") + b"\0"
    reply = b""
    try:
        with socket.create_connection(("127.0.0.1", PORT), timeout=timeout) as connection:
            connection.sendall(struct.pack("<I", len(body)) + body)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline and b"executed successfully" not in reply and b"Error in " not in reply:
                data = connection.recv(65536)
                if not data:
                    break
                reply += data
    except OSError as error:
        return False, (f"the game's Lua runner did not answer ({error}): "
                       "is the game running with vscodelua.asi, in a world?")
    finally:
        try:
            os.remove(path)
        except OSError:
            pass
    text = reply.decode("utf-8", "replace")
    return "executed successfully" in text, text.strip()


def lua_string(text):
    """@p text as a double-quoted Lua string literal."""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "\\r") + '"'


class LogWatch:
    """New lines of the framework's log from the moment it is made."""

    def __init__(self, path):
        self.path = path
        try:
            self.offset = os.path.getsize(path)
        except OSError:
            self.offset = 0

    def poll(self, needle):
        """The first new line holding @p needle, or None when none is written yet; the next poll starts after it."""
        try:
            with open(self.path, "rb") as log:
                log.seek(self.offset)
                data = log.read()
        except OSError:
            return None
        position = 0
        for raw in data.splitlines(keepends=True):
            if not raw.endswith(b"\n"):
                break  # a line the game has not finished
            position += len(raw)
            if needle.encode("utf-8") in raw:
                self.offset += position
                return raw.decode("utf-8", "replace").rstrip()
        return None
