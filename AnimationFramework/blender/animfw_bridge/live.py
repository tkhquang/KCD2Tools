"""Talks to the running game through the Lua runner (vscodelua.asi, github.com/yobson1/kcd2lua) and waits for the
Animation Framework's log to confirm each step.

The runner listens on 127.0.0.1:28771 and takes a little-endian u32 length, then comma-separated absolute paths of .lua
files and a NUL; it runs them on the game thread once a world is loaded and answers "Files queued for execution", then
"All scripts executed successfully" or "Error in <file>: <message>".
"""
import os
import socket
import struct
import tempfile
import time

PORT = 28771


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
