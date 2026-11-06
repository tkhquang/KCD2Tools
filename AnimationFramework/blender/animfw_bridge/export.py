"""Samples a KCD2 armature's action at the game's 30 frames a second and writes it as an intermediate animation
(.i_caf), the file animfw.exe compiles into a game clip.

The armature is one Bohemian Animator builds from the game's skeleton: a bone's joint transform in the game is its
pose matrix times the axis swap below, and bones marked kcd2_ik_control are its IK helpers, not game bones.
"""
import math
import struct
import zlib

from mathutils import Matrix

# Bohemian Animator's bone axis convention (rig.py BCRY_SWAP): game joint = pose bone matrix @ SWAP.
SWAP = Matrix(((0.0, 1.0, 0.0, 0.0), (1.0, 0.0, 0.0, 0.0), (0.0, 0.0, -1.0, 0.0), (0.0, 0.0, 0.0, 1.0)))
CONTROL_PROPERTY = "kcd2_ik_control"
GAME_FPS = 30.0
TICKS_PER_SECOND = 4800
TICKS_PER_FRAME = TICKS_PER_SECOND // int(GAME_FPS)

# Chunk types and versions of the intermediate format (type in the low 16 bits, version in the high 16).
CHUNK_EXPORT_FLAGS = 0x00011015
CHUNK_TIMING = 0x0918100E
CHUNK_CONTROLLER = 0x0827100D
CHUNK_BONE_NAMES = 0x07451005


def game_bones(armature):
    """The armature's game bones, parents before children."""
    return [b.name for b in armature.data.bones if not b.get(CONTROL_PROPERTY)]


def frame_range(context, armature, source):
    """(first, last) scene frame of the clip: the action's keys, or the scene's range."""
    if source == "ACTION":
        action = armature.animation_data.action if armature.animation_data else None
        if action is not None:
            first, last = action.frame_range
            return float(first), float(last)
    return float(context.scene.frame_start), float(context.scene.frame_end)


def sample(context, armature, first, last):
    """Each game bone's transform relative to its parent at every game frame from @p first to @p last (scene
    frames), sampled at 30 a second whatever the scene's frame rate. Returns (bone names, {name: [Matrix]})."""
    scene = context.scene
    fps = scene.render.fps / scene.render.fps_base
    count = int(round((last - first) / fps * GAME_FPS)) + 1
    names = game_bones(armature)
    known = set(names)
    parent = {}
    for name in names:
        bone = armature.data.bones[name]
        # The intermediate file stores names and their CRC-32 controller ids as ASCII, as the game's skeleton does.
        if not name.isascii():
            raise ValueError(f"{name} is not an ASCII name, which a game bone needs")
        if bone.parent is not None and bone.parent.name not in known:
            raise ValueError(f"{name} is parented to {bone.parent.name}, which is not a game bone")
        parent[name] = bone.parent.name if bone.parent is not None else None
    tracks = {name: [] for name in names}
    saved = (scene.frame_current, scene.frame_subframe)
    try:
        for k in range(count):
            t = first + k * fps / GAME_FPS
            whole = math.floor(t)
            scene.frame_set(int(whole), subframe=t - whole)
            world = {name: armature.pose.bones[name].matrix @ SWAP for name in names}
            for name in names:
                p = parent[name]
                tracks[name].append((world[name] if p is None else world[p].inverted() @ world[name]).copy())
    finally:
        scene.frame_set(saved[0], subframe=saved[1])
    return names, tracks


def _chunk_file(chunks):
    """A CrCh 0x746 chunk file from (type|version, id, payload) entries."""
    header = b"CrCh" + struct.pack("<III", 0x746, len(chunks), 16)
    offset = 16 + 16 * len(chunks)
    table, body = [], []
    for kind, ident, payload in chunks:
        table.append(struct.pack("<IIII", kind, ident, len(payload), offset))
        body.append(payload)
        offset += len(payload)
    return header + b"".join(table) + b"".join(body)


def _rotation_log(q):
    """log of a unit quaternion (w, x, y, z): half the angle times the axis."""
    w, x, y, z = q
    n = math.sqrt(x * x + y * y + z * z)
    if n <= 0.0:
        return 0.0, 0.0, 0.0
    a = math.atan2(n, w) / n
    return x * a, y * a, z * a


def write_intermediate(path, names, tracks):
    """Writes the sampled tracks as an .i_caf: per bone, keys of {tick, position in cm, log of the conjugate
    rotation}, as the Resource Compiler and animfw.exe read them."""
    count = len(next(iter(tracks.values()))) if tracks else 0
    flags = bytearray(164)
    timing = bytearray(52)
    struct.pack_into("<fi", timing, 0, 1.0 / TICKS_PER_SECOND, TICKS_PER_FRAME)
    timing[8:8 + len(b"GlobalRange")] = b"GlobalRange"
    struct.pack_into("<ii", timing, 40, 1, count)
    chunks = [(CHUNK_EXPORT_FLAGS, 1, bytes(flags)), (CHUNK_TIMING, 2, bytes(timing))]
    for index, name in enumerate(names):
        keys = [struct.pack("<II", len(tracks[name]), zlib.crc32(name.encode("ascii")) & 0xFFFFFFFF)]
        previous = None
        for k, matrix in enumerate(tracks[name]):
            location, rotation, _scale = matrix.decompose()
            q = (rotation.w, -rotation.x, -rotation.y, -rotation.z)  # the conjugate, as the format stores it
            if previous is not None and sum(a * b for a, b in zip(q, previous)) < 0.0:
                q = tuple(-c for c in q)
            previous = q
            keys.append(struct.pack("<i3f3f", (k + 1) * TICKS_PER_FRAME, location.x * 100.0, location.y * 100.0,
                                    location.z * 100.0, *_rotation_log(q)))
        chunks.append((CHUNK_CONTROLLER, 3 + index, b"".join(keys)))
    names_payload = struct.pack("<i", len(names)) + b"".join(n.encode("ascii") + b"\0" for n in names) + b"\0"
    chunks.append((CHUNK_BONE_NAMES, 3 + len(names), names_payload))
    with open(path, "wb") as out:
        out.write(_chunk_file(chunks))
