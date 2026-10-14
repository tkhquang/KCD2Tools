"""Build TPVCamera's shader twins (the tree see-through and the character fade) and embed them as a C++ header.

TPVCamera opens a dithered window in the trees between the camera and the character. Leaves and most trunks are
drawn by the stock Vegetation shader, other trunks, bark and decals of the same trees by the stock Illum shader (a
willow's bark, rotten or burned trees, branch piles). Both take their Z pre-pass and G-buffer pass from
CommonZPrePass.cfi and CommonZPass.cfi. The retail game only loads pre-tokenized shaders (see cfxb.py), so the window
is added by splicing the game's own binaries at the token level; nothing is re-tokenized from source except the
mod's small hole function:

    kcd2_tpvcamera_hole_<crc>.cfib           shaders/TPVCameraHole.cfi (TPVHoleAmount, TPVApplyHole)
    kcd2_tpvcamera_commonzprepass_<crc>.cfib commonzprepass.cfib with TPVApplyHole(IN.WPos); after its
    kcd2_tpvcamera_commonzpass_<crc>.cfib    commonzpass.cfib    ApplyDissolve(...) call, and the
                                                                 CRY_EARLY_DEPTH_OR_RE_Z keyword in front of the
                                                                 pixel shader removed (the new clip() needs late depth)
    kcd2_tpvcamera_vegetation_<crc>.cfxb     vegetation.cfxb and illum.cfxb, each with the hole included right after
    kcd2_tpvcamera_illum_<crc>.cfxb          ShadeLib and its two Z pass includes renamed to the copies above

The hole also cuts what is close to the lens. The bare tent frames (beams, poles) draw with Illum too, and should lose
only that, not the window around the character, so they get a third copy of illum.cfxb with a near-only hole include.
The generator removes the window's TPVCAMERA_FRAME_HOLE conditional block from that include. This preserves the
near cut without putting a frame macro into every hardware vertex program, where it would prevent exact sharing:

    kcd2_tpvcamera_nearhole_<crc>.cfib       TPVCameraHole.cfi with its window block removed
    kcd2_tpvcamera_frame_<crc>.cfxb          illum.cfxb, as the Illum twin, including the near-only helper

The close-up fade dithers the character out, but three stock shaders draw parts of him again in forward passes whose
pixel shaders never call ApplyDissolve (only their G-buffer and Z passes do): Eye (the eyes, and the eye's AO
overlay), Hair (the eyelashes, the beard, the hair and a hood's hair cards, alpha-blended after the G-buffer) and
Illum on a transparent item (the wet film over the eyes). Their twins have no hole and keep the stock includes; each
adds the dissolve test to its forward pixel shaders and changes nothing else: ApplyDissolve's own test on
Get_SPI_Dissolve(), but not behind %_RT_DISSOLVE, which the engine takes out of the forward pass of the items its Z
prepass drew. The per-draw dissolve is 0 while nothing dissolves, so a twin then draws like the stock shader:

    kcd2_tpvcamera_eye_<crc>.cfxb            eye.cfxb, the call (on IN.WPos) as the first statement of EyePS and
                                             EyeOverlayPS
    kcd2_tpvcamera_hair_<crc>.cfxb           hair.cfxb, the call (on IN.Common.WPos, vert2FragHair's
                                             vert2FragGeneral) as the first statement of HairPS (General's back
                                             pass, HairFrontPass and HairBackPass) and HairOpaquePS (General's
                                             opaque pass)
    kcd2_tpvcamera_illumfade_<crc>.cfxb      illum.cfxb, the call (on IN.WPos) in IlluminationPS right after its
                                             parallax occlusion fix-up (#if %SILHOUETTE_PARALLAX_OCCLUSION_MAPPING
                                             ... #endif), where IN exists with either signature; the DLL swaps it
                                             only on a transparent item (opacity below 1)

The header also carries the constants of the DEV twin clone (src/hooks/shader_token_splice.hpp), which derives a hole
twin's Z-pass pixel shaders from the stock ones without parsing the twin: the token patterns it splices at, the hole
include's tokens as a twin's pixel shader carries them, Common.cfi's GetWorldViewPos fragment, and the string-table
entries the twin adds and drops.

The shader names are KCD2_TPVCamera_<kind>_<crc> (Vegetation, Illum, Frame, Eye, Hair, IllumFade). The engine
lower-cases a shader's name for its file, so every file starts with kcd2_tpvcamera_, the prefix of every file the mod
keeps beside the ASI. The DLL writes the files to
the user shader cache and creates a twin with CShaderMan::mfForName for each source instance it swaps (on a tree or a
frame it lists, or on the dissolving character). Every <crc> is the file's own header CRC, so any change to the hole
function or to the game's shaders gives new names (the engine keeps a loaded shader by name for the session).

Header CRC (the engine's sub_180918A8C, checked on every load): CRC-32 of the token array plus, for every #include
token, the same value computed for the included .cfib, recursively. The script proves its routine first: the
computed CRC of every binary in the pak must equal the one stored in its header.

Every splice point is found by its token pattern and must occur exactly once, else the script stops with an error
and writes nothing. Each tree twin's whole include tree is then checked: its ZPass and ZPrePass techniques and their
pixel shaders exist only in the copies, and no ApplyDissolve call outside the copies could draw a pass the swap
reaches (the shadow pass keeps the stock shader). The character twins' are checked too: every pixel shader their
forward techniques name starts with the dissolve, nothing else in the twin differs from its stock file, and nothing in
its tree opens the window. The header CRCs of the stock files go into the C++ header, so the DLL can turn a twin, or
the whole feature, off when the game's shaders change.

For testing a true first run (no compiled permutation of any twin in the user shader cache), --name-salt <text> puts
"#define TPVCAMERA_NAME_SALT_<text> 1" (a macro nothing reads) at the top of every twin, which changes every twin's CRC
and so every twin's name; the DLL then deletes the old names' files and compiled entries and the engine compiles the
new ones from scratch. Without it every twin keeps the name its token stream gives it.

Usage:
    py -3 scripts/build_shader_twins.py [--pak <ShadersBin.pak>] [--header <out.hpp>] [--out-dir <dir>]
                                     [--name-salt <text>]
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import zipfile
import zlib
from typing import NamedTuple

sys.dont_write_bytecode = True  # no __pycache__ next to the scripts
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cfxb  # noqa: E402

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PAK = "D:/Games/SteamLibrary/steamapps/common/KingdomComeDeliverance2/Engine/ShadersBin.pak"
DEFAULT_HEADER = os.path.join(REPO_DIR, "src", "generated", "shader_twin_blobs.hpp")
HOLE_SOURCE = os.path.join(REPO_DIR, "shaders", "TPVCameraHole.cfi")
PAK_SHADER_DIR = "shaders/cache/d3d12/"

# Every shader name starts with the mod's name, so every file it writes to the user shader cache (the engine lower-cases
# the name for the file) starts with FILE_PREFIX.
NAME_PREFIX = "KCD2_TPVCamera_"
FILE_PREFIX = NAME_PREFIX.lower()
NAME_HOLE = NAME_PREFIX + "Hole"
NAME_NEAR_HOLE = NAME_PREFIX + "NearHole"
NAME_ZPREPASS = NAME_PREFIX + "CommonZPrePass"
NAME_ZPASS = NAME_PREFIX + "CommonZPass"

# What the DLL swaps to a twin (the TwinScope enumerators of the C++ header): the items of the plants the camera
# lists, the items of the bare frames it lists, or the items of a temporary render object of the player's character
# that dissolves (the close-up fade).
SCOPE_PLANTS = "ListedPlants"
SCOPE_FRAMES = "ListedFrames"
SCOPE_DISSOLVING = "Dissolving"
# The scopes whose twins include the hole.
SCOPES_HOLE = (SCOPE_PLANTS, SCOPE_FRAMES)
# The source conditional specialized by the generator for frames; never defined in their shader token streams.
FRAME_MACRO = b"TPVCAMERA_FRAME_HOLE"

# Token patterns, tokenized here with the game's rules so they match the shipped binaries exactly.
# The stock dissolve call of the Z passes, after which the copies call the hole.
DISSOLVE_CALL = b"ApplyDissolve(Get_SPI_Dissolve(), (int2)IN.WPos.xy);"
# The character twins' dissolve: the stock ApplyDissolve's test (TestDissolve: the 4x4 threshold against the per-draw
# dissolve, its sign picking in or out), but not behind %_RT_DISSOLVE. The forward pass of the items the Z prepass drew
# (sub_1807C5A20's pass 1, 0x1807C5B50 and 0x1807C5E41) takes the dissolve bit out of the runtime mask it compiles, so
# ApplyDissolve compiles to nothing there and leaves the eyes and the eye film solid over the faded character.
# Get_SPI_Dissolve() reads the per-draw constant every pass sees; it is 0 while nothing dissolves, and a 0 clips
# nothing (the threshold is never below 0).
FADE_CALL = (b"{ const float tpvFade = Get_SPI_Dissolve(); clip((tpvFade >= 0 ? 1.0 : -1.0) * "
                 b"(GetDissolveThreshold17((int2)IN.WPos.xy) - abs(tpvFade))); }")
# The same where the pixel shader's input is vert2FragHair, whose vert2FragGeneral is its member Common. Hair's
# General pass fills IN.Common.baseTC from HairVS, so the dissolve its own Z passes read from baseTC.z is not there.
FADE_CALL_HAIR = (b"{ const float tpvFade = Get_SPI_Dissolve(); clip((tpvFade >= 0 ? 1.0 : -1.0) * "
                      b"(GetDissolveThreshold17((int2)IN.Common.WPos.xy) - abs(tpvFade))); }")


class Insert(NamedTuple):
    """One dissolve call a character twin adds, right after anchor (which must occur exactly once in the stock file):
    the first statement of the pixel shader ps, or the first after prelude when its body opens with that."""

    anchor: bytes
    call: bytes
    ps: str
    prelude: bytes = b""


class Twin(NamedTuple):
    """A stock shader that gets a twin. label names the kind (the twin's name is KCD2_TPVCamera_<label>_<crc>);
    source_name is the name the engine knows the stock shader by (and loads its .ext with), source_file its binary in
    the pak. A ListedPlants twin gets the window and the near cut, a ListedFrames twin the near cut only. A
    Dissolving twin gets inserts, which must cover every pixel shader the techniques in forward name; transparent_only
    tells the DLL to swap it only on an item whose opacity is below 1 (the engine's own test for the transparent
    list)."""

    label: str
    source_name: str
    source_file: str
    scope: str
    transparent_only: bool = False
    forward: tuple[str, ...] = ()
    inserts: tuple[Insert, ...] = ()


# Eye.cfx and Hair.cfx define HIDING_GROUPS, which drops the early-depth attribute in front of their pixel shaders, and
# the engine expands IlluminationPS's CRY_EARLY_DEPTH to nothing in a permutation with the dissolve, so a clip() keeps
# its meaning in each. Hair's chained HairFrontPass and HairBackPass (the multi-pass hair of the beard and the hood)
# name HairPS too; ThinHairDepthPS is named by no technique. IlluminationPS takes geom2FragGeneral IN_ext under
# %SILHOUETTE_PARALLAX_OCCLUSION_MAPPING and copies its IN out first; the call goes after that copy, so IN exists with
# either signature.
TWINS = (
    Twin("Vegetation", "Vegetation", "vegetation.cfxb", SCOPE_PLANTS),
    Twin("Illum", "Illum", "illum.cfxb", SCOPE_PLANTS),
    Twin("Frame", "Illum", "illum.cfxb", SCOPE_FRAMES),
    Twin("Eye", "Eye", "eye.cfxb", SCOPE_DISSOLVING, forward=("General",), inserts=(
        Insert(b"pixout EyePS(vert2FragGeneral IN)\n{", FADE_CALL, "EyePS"),
        Insert(b"min16float2 EyeOverlayPS(vert2FragGeneral IN) : SV_Target0\n{", FADE_CALL, "EyeOverlayPS"),
    )),
    Twin("Hair", "Hair", "hair.cfxb", SCOPE_DISSOLVING, forward=("General", "HairFrontPass", "HairBackPass"), inserts=(
        Insert(b"pixoutHair2 HairPS(vert2FragHair IN)\n{", FADE_CALL_HAIR, "HairPS"),
        Insert(b"pixoutHair HairOpaquePS(vert2FragHair IN)\n{", FADE_CALL_HAIR, "HairOpaquePS"),
    )),
    Twin("IllumFade", "Illum", "illum.cfxb", SCOPE_DISSOLVING, transparent_only=True, forward=("General",), inserts=(
        Insert(b"vert2FragGeneral IN = IN_ext.IN;\n#endif", FADE_CALL, "IlluminationPS",
               prelude=b"#if %SILHOUETTE_PARALLAX_OCCLUSION_MAPPING\nvert2FragGeneral IN = IN_ext.IN;\n#endif"),
    )),
)
HOLE_CALL = b"TPVApplyHole(IN.WPos);"
EARLY_DEPTH = b"CRY_EARLY_DEPTH_OR_RE_Z"
ZPASS_PS = b"pixout_gbuffer Common_ZPassPS("
ZPREPASS_PS = b"pixout_zpre Common_ZPrePassPS("
# The Z pass techniques the swapped items draw, and the pixel shader each runs (see the copies).
ZPASS_TECHNIQUES = (("ZPass", "Common_ZPassPS"), ("ZPrePass", "Common_ZPrePassPS"))
# Includes whose ApplyDissolve call needs no hole: the shadow-map pass, which a swapped item never draws with the
# twin (the swap is main view only).
UNSWAPPED_PASS_INCLUDES = ("commonshadowgenpass",)
# The DEV twin clone splices the stock Z-pass pixel shader's token stream (its SHData: the code fragments it uses, in
# file order) like the pass copies and the twin's includes do: the hole call after ApplyDissolve, no early-depth keyword
# in front of the signature, the hole include's fragments right after ShadeLib's last function, and, when the full
# hole calls GetWorldViewPos and the stock stream does not emit it, Common.cfi's fragment right after the emitted
# fragment in front of it there (in each pass). The hole's other dependencies must already be emitted. The clone fails
# closed when an anchor is not unique.
SPLICE_HOLE_AFTER = b"GetAlphaCoverageMask"
SPLICE_WORLD_VIEW_POS = b"GetWorldViewPos"
SPLICE_ZPREPASS_WORLD_VIEW_POS_AFTER = b"USE_DEVICE_DEPTH"  # textureLib.cfi's, included by Common.cfi before it
SPLICE_ZPASS_WORLD_VIEW_POS_AFTER = b"OutputVelocityRT"
SPLICE_REQUIRED = (b"CBPerViewGlobal", b"GetDissolveThreshold17")
# The include names a twin's string table drops for the copies' names.
SPLICE_TABLE_REMOVED = ("CommonZPrePass", "CommonZPass")


class BuildError(Exception):
    """A missing or ambiguous splice point, or a pak that does not look like the one this was written for."""


def salt_define(salt: str) -> tuple[list[int], dict[int, bytes]]:
    """The tokens of "#define TPVCAMERA_NAME_SALT_<salt> 1", put at the top of every twin by --name-salt, or none.
    The macro is read by nothing, so the twin compiles to the same code; only its tokens, and so its CRC and name,
    change."""
    if not salt:
        return [], {}
    if not re.fullmatch(r"[A-Za-z0-9_]{1,32}", salt):
        raise BuildError("--name-salt takes 1 to 32 letters, digits or underscores, not %r" % salt)
    return cfxb.tokenize(b"#define TPVCAMERA_NAME_SALT_%s 1\n" % salt.encode("latin-1"))


def include_tokens(name: str) -> list[int]:
    toks, _ = cfxb.tokenize(b'#include "%s.cfi"\n' % name.encode("latin-1"))
    return toks


def find_all(tokens: list[int], run: list[int]) -> list[int]:
    n = len(run)
    first = run[0]
    return [i for i in range(len(tokens) - n + 1) if tokens[i] == first and tokens[i : i + n] == run]


def find_unique(tokens: list[int], run: list[int], file_name: str, what: str) -> int:
    hits = find_all(tokens, run)
    if len(hits) != 1:
        raise BuildError("%s: %s found %d times (expected exactly once)" % (file_name, what, len(hits)))
    return hits[0]


def prune_table(tokens: list[int], table: dict[int, bytes], file_name: str) -> dict[int, bytes]:
    """The string table a fresh tokenization would write: one entry per distinct non-keyword token."""
    used = {t for t in tokens if t >= cfxb.KEYWORD_COUNT}
    missing = sorted(used - table.keys())
    if missing:
        raise BuildError("%s: %d token(s) without a string, e.g. 0x%08X" % (file_name, len(missing), missing[0]))
    return {t: table[t] for t in used}


class PakResolver(cfxb.IncludeResolver):
    """Resolves #include names like the engine's GetBinShader(name, bInclude=true): the .cfib of that name, here
    from the pak (read into memory) or from the files this script builds."""

    def __init__(self, binaries: dict[str, bytes]):
        super().__init__(())
        self.binaries = binaries
        self.built: dict[str, tuple[list[int], dict[int, bytes]]] = {}

    def add(self, name: str, tokens: list[int], table: dict[int, bytes]) -> None:
        self.built[name.lower()] = (tokens, table)
        self.cache.pop(name.lower(), None)
        self.crc_cache.pop(name.lower(), None)

    def load(self, name: str):
        key = name.lower()
        if key in self.built:
            return self.built[key]
        if key not in self.cache:
            data = self.binaries.get(key + ".cfib")
            if data is None:
                self.cache[key] = None
                self.missing.add(name)
            else:
                sb = cfxb.parse(data)
                self.cache[key] = (sb["tokens"], sb["strings"])
        return self.cache[key]


def read_pak(path: str) -> dict[str, bytes]:
    """Every shader binary in ShadersBin.pak, keyed by its lower-case file name."""
    if not os.path.isfile(path):
        raise BuildError("%s not found (pass --pak)" % path)
    out: dict[str, bytes] = {}
    with zipfile.ZipFile(path) as pak:
        for info in pak.infolist():
            low = info.filename.replace("\\", "/").lower()
            if low.startswith(PAK_SHADER_DIR) and low.endswith((".cfxb", ".cfib")):
                out[low[len(PAK_SHADER_DIR) :]] = pak.read(info)
    if not out:
        raise BuildError("%s holds no %s*.cfxb/.cfib" % (path, PAK_SHADER_DIR))
    return out


def stock_crc(resolver: PakResolver, file_name: str, sb: dict) -> int:
    stack = (file_name.rsplit(".", 1)[0],) if file_name.endswith(".cfib") else ()
    return cfxb.compute_crc(sb["tokens"], sb["strings"], resolver, stack)


def check_stock(binaries: dict[str, bytes], resolver: PakResolver) -> dict[str, dict]:
    """Parse every pak binary, check its format, and prove the CRC routine on all of them."""
    parsed: dict[str, dict] = {}
    bad: list[str] = []
    for file_name in sorted(binaries):
        sb = cfxb.parse(binaries[file_name])
        h = sb["header"]
        if (h["magic"], h["version_low"], h["version_high"]) != (cfxb.MAGIC, cfxb.VERSION_LOW, cfxb.VERSION_HIGH):
            raise BuildError("%s: header %r %d.%d, expected %r %d.%d" % (
                file_name, h["magic"], h["version_high"], h["version_low"], cfxb.MAGIC, cfxb.VERSION_HIGH,
                cfxb.VERSION_LOW))
        if stock_crc(resolver, file_name, sb) != h["crc"]:
            bad.append(file_name)
        parsed[file_name] = sb
    if bad:
        raise BuildError("the header CRC routine disagrees with %d pak file(s): %s" % (len(bad), ", ".join(bad[:8])))
    print("pak: %d shader binaries, every header CRC reproduced" % len(parsed))
    return parsed


class Splice:
    """A copy of a stock token stream being edited; every edit is logged with the tokens in front of it."""

    def __init__(self, sb: dict, file_name: str):
        self.file_name = file_name
        self.tokens = list(sb["tokens"])
        self.table = dict(sb["strings"])
        self.log: list[str] = []

    def _text(self, tokens: list[int]) -> str:
        return " ".join(cfxb.token_text(t, self.table) for t in tokens)

    def replace(self, at: int, count: int, new: list[int], new_table: dict[int, bytes], op: str) -> None:
        self.table.update(new_table)
        before = self._text(self.tokens[max(0, at - 6) : at])
        old = self._text(self.tokens[at : at + count])
        self.tokens[at : at + count] = new
        self.log.append("  %s: %s at token %d, after '... %s': [%s] -> [%s]"
                        % (self.file_name, op, at, before, old, self._text(new)))

    def finish(self) -> tuple[list[int], dict[int, bytes]]:
        return self.tokens, prune_table(self.tokens, self.table, self.file_name)


def splice_pass(sb: dict, file_name: str, ps_signature: bytes, log: list[str]) -> tuple[list[int], dict[int, bytes]]:
    """A Z pass copy: the hole call after ApplyDissolve, and late depth for its pixel shader."""
    edit = Splice(sb, file_name)

    dissolve, _ = cfxb.tokenize(DISSOLVE_CALL)
    if len(dissolve) != 16:
        raise BuildError("the ApplyDissolve call tokenized to %d tokens, expected 16" % len(dissolve))
    hole_call, hole_table = cfxb.tokenize(HOLE_CALL)
    if len(hole_call) != 7:
        raise BuildError("the TPVApplyHole call tokenized to %d tokens, expected 7" % len(hole_call))
    if find_all(edit.tokens, hole_call[:1]):
        raise BuildError("%s already calls TPVApplyHole (not a stock file)" % file_name)

    at = find_unique(edit.tokens, dissolve, file_name,
                     "the ApplyDissolve(Get_SPI_Dissolve(), (int2)IN.WPos.xy); call")
    edit.replace(at + len(dissolve), 0, hole_call, hole_table, "insert")

    early = cfxb.KEYWORD_ID[EARLY_DEPTH]
    signature, _ = cfxb.tokenize(ps_signature)
    at = find_unique(edit.tokens, [early] + signature, file_name,
                     "%s in front of '%s'" % (EARLY_DEPTH.decode(), ps_signature.decode()))
    edit.replace(at, 1, [], {}, "delete")
    log += edit.log
    return edit.finish()


def splice_twin(sb: dict, file_name: str, hole_name: str, zprepass_name: str, zpass_name: str,
                log: list[str]) -> tuple[list[int], dict[int, bytes]]:
    """A source shader's copy: the hole included right after ShadeLib, the two Z pass includes renamed to the
    copies."""
    edit = Splice(sb, file_name)

    shadelib = include_tokens("ShadeLib")
    at = find_unique(edit.tokens, shadelib, file_name, '#include "ShadeLib.cfi"')
    hole_include, hole_table = cfxb.tokenize(b'#include "%s.cfi"\n' % hole_name.encode("latin-1"))
    edit.replace(at + len(shadelib), 0, hole_include, hole_table, "insert")

    for stock, copy in (("CommonZPrePass", zprepass_name), ("CommonZPass", zpass_name)):
        stock_include = include_tokens(stock)
        at = find_unique(edit.tokens, stock_include, file_name, '#include "%s.cfi"' % stock)
        new_include, new_table = cfxb.tokenize(b'#include "%s.cfi"\n' % copy.encode("latin-1"))
        edit.replace(at, len(stock_include), new_include, new_table, "replace")
    log += edit.log
    return edit.finish()


def splice_forward(sb: dict, file_name: str, inserts: tuple[Insert, ...],
                   log: list[str]) -> tuple[list[int], dict[int, bytes]]:
    """A character twin's copy: each insert's dissolve call right after its anchor. A stock file that already makes
    one of the calls is refused; other ApplyDissolve calls (Hair's own Z passes read baseTC.z) stay."""
    edit = Splice(sb, file_name)
    for insert in inserts:
        call, _ = cfxb.tokenize(insert.call)
        if find_all(edit.tokens, call):
            raise BuildError("%s already makes the call %s (not the stock file this was written for)"
                             % (file_name, insert.call.decode()))
    for insert in inserts:
        call, call_table = cfxb.tokenize(insert.call)
        anchor, _ = cfxb.tokenize(insert.anchor)
        at = find_unique(edit.tokens, anchor, file_name, "'%s'" % insert.anchor.decode().replace("\n", " "))
        edit.replace(at + len(anchor), 0, call, call_table, "insert")
    log += edit.log
    return edit.finish()


def frame_hole(tokens: list[int], table: dict[int, bytes]) -> tuple[list[int], dict[int, bytes]]:
    """Resolve the hole's single window conditional for a frame, keeping all other tokens exactly.

    This is a deliberately narrow specialization, not a general shader preprocessor. Unexpected directives or
    other uses of the frame macro fail generation rather than silently changing a future shader's meaning.
    """
    opening, _ = cfxb.tokenize(b"#if !" + FRAME_MACRO + b"\n")
    ending = cfxb.KEYWORD_ID[b"#endif"]
    directives = {token for token, text in enumerate(cfxb.KEYWORDS) if text and text.startswith("#")}
    if [token for token in tokens if token in directives] != [opening[0], ending]:
        raise BuildError("the frame hole requires exactly one #if !TPVCAMERA_FRAME_HOLE block, with no other directives")
    at = find_unique(tokens, opening, "frame hole", "#if !TPVCAMERA_FRAME_HOLE")
    if tokens.count(word_token(FRAME_MACRO)) != 1:
        raise BuildError("the frame macro is used outside the hole's window conditional")
    end = tokens.index(ending, at + len(opening))
    specialized = tokens[:at] + tokens[end + 1:]
    return specialized, prune_table(specialized, table, "frame hole")


def word_token(text: bytes) -> int:
    """The token a single word becomes (a keyword id or the CRC of its text)."""
    tokens, _ = cfxb.tokenize(text)
    if len(tokens) != 1:
        raise BuildError("'%s' is not a single token" % text.decode("latin-1"))
    return tokens[0]


def check_coverage(file_name: str, tokens: list[int], table: dict[int, bytes], resolver: PakResolver,
                   copies: dict[str, str]) -> str:
    """Proves on a twin's whole include tree that the hole reaches every pixel shader the swap draws.

    Every include counts, whatever #if it sits under, so no permutation can pick another file. The ZPass and
    ZPrePass techniques must be defined once, in their copy, and their pixel shaders named nowhere else. Every
    ApplyDissolve call must be followed by the hole call, or sit in a pass the swap never draws with the twin; the
    only other ApplyDissolve allowed is ShadeLib's definition. The copies keep no early-depth keyword.

    copies maps a technique name to the include name of the copy that must hold it. Returns a summary for the log.
    """
    t_technique = word_token(b"technique")
    t_void = word_token(b"void")
    t_apply = word_token(b"ApplyDissolve")
    t_semicolon = word_token(b";")
    t_early = (cfxb.KEYWORD_ID[EARLY_DEPTH], cfxb.KEYWORD_ID[b"CRY_EARLY_DEPTH"])
    hole_call, _ = cfxb.tokenize(HOLE_CALL)

    # Each file once (the engine's own CRC counts repeats, coverage does not).
    files = include_tree(file_name, tokens, table, resolver)

    defined: dict[str, list[str]] = {}
    covered = 0
    unswapped = 0
    definitions = 0
    for name, (own_tokens, own_table) in files.items():
        for i, tok in enumerate(own_tokens[:-1]):
            if tok == t_technique:
                defined.setdefault(cfxb.token_text(own_tokens[i + 1], own_table), []).append(name)
        for i, tok in enumerate(own_tokens):
            if tok != t_apply:
                continue
            if i > 0 and own_tokens[i - 1] == t_void:
                definitions += 1
                continue
            try:
                end = own_tokens.index(t_semicolon, i) + 1
            except ValueError:
                raise BuildError("%s: an ApplyDissolve call in %s has no end" % (file_name, name)) from None
            if own_tokens[end : end + len(hole_call)] == hole_call:
                covered += 1
            elif name in UNSWAPPED_PASS_INCLUDES:
                unswapped += 1
            else:
                raise BuildError("%s: an ApplyDissolve call in %s, at token %d, has no hole after it; a pass the "
                                 "swap draws would keep those pixels" % (file_name, name, i))

    for technique, shader in ZPASS_TECHNIQUES:
        copy = copies[technique].lower()
        where = defined.get(technique, [])
        if where != [copy]:
            raise BuildError("%s: technique %s is defined in %s, expected only in %s"
                             % (file_name, technique, ", ".join(where) or "no file", copy))
        t_shader = word_token(shader.encode("latin-1"))
        elsewhere = sorted(n for n, (own, _) in files.items() if n != copy and t_shader in own)
        if elsewhere:
            raise BuildError("%s: %s is also named in %s" % (file_name, shader, ", ".join(elsewhere)))
        if any(t in files[copy][0] for t in t_early):
            raise BuildError("%s: %s still has an early-depth keyword" % (file_name, copy))
    if covered != len(ZPASS_TECHNIQUES) or definitions != 1:
        raise BuildError("%s: %d ApplyDissolve call(s) with the hole and %d definition(s), expected %d and 1"
                         % (file_name, covered, definitions, len(ZPASS_TECHNIQUES)))
    return ("%s: %d include files; ZPass and ZPrePass only in the copies; ApplyDissolve: %d call(s) with the hole, "
            "%d in the shadow pass, ShadeLib's definition; techniques %s"
            % (file_name, len(files) - 1, covered, unswapped, ", ".join(sorted(defined))))


def check_forward_coverage(file_name: str, tokens: list[int], table: dict[int, bytes], resolver: PakResolver,
                           twin: Twin, stock_tokens: list[int], salt: list[int]) -> str:
    """Proves on a character twin's whole include tree that its forward pixel shaders dissolve and nothing else
    changed.

    The tree must not include any of the mod's files nor call the hole. Every technique in twin.forward must exist in
    the twin's own stream, and the pixel shaders they name (PixelShader = <name>(...)) must be exactly the inserts'.
    Each of those has one body, in the twin, is named by no include, and its insert's call opens that body (after the
    insert's prelude, if any). Taken out the calls and the salt macro at the top (--name-salt), the twin is its stock
    file token for token, so every other technique (its Z passes, shadow, motion blur and custom passes, and the
    includes') draws like the stock shader. Returns a summary for the log.
    """
    t_technique = word_token(b"technique")
    t_pixel_shader = word_token(b"PixelShader")
    t_equals = word_token(b"=")
    t_open_paren = word_token(b"(")
    t_open_brace = word_token(b"{")
    t_close_brace = word_token(b"}")
    t_semicolon = word_token(b";")
    hole_call, _ = cfxb.tokenize(HOLE_CALL)

    files = include_tree(file_name, tokens, table, resolver)
    for name in files:
        if name != file_name.lower() and name.startswith(FILE_PREFIX):
            raise BuildError("%s: includes the mod's %s; a character twin must not open the window"
                             % (file_name, name))
    for name, (own_tokens, _) in files.items():
        if find_all(own_tokens, hole_call[:1]):
            raise BuildError("%s: %s calls the hole" % (file_name, name))

    # The pixel shaders each technique of the twin's own stream names.
    named_by: dict[str, set[str]] = {}
    technique = None
    for i, tok in enumerate(tokens[:-2]):
        if tok == t_technique:
            technique = cfxb.token_text(tokens[i + 1], table)
            named_by.setdefault(technique, set())
        elif tok == t_pixel_shader and tokens[i + 1] == t_equals and technique is not None:
            named_by[technique].add(cfxb.token_text(tokens[i + 2], table))
    missing = [t for t in twin.forward if t not in named_by]
    if missing:
        raise BuildError("%s: has no technique %s" % (file_name, ", ".join(missing)))
    named = sorted(set().union(*(named_by[t] for t in twin.forward)))
    wanted = sorted({insert.ps for insert in twin.inserts})
    if named != wanted:
        raise BuildError("%s: its techniques %s name the pixel shaders %s, expected %s"
                         % (file_name, ", ".join(twin.forward), ", ".join(named) or "none", ", ".join(wanted)))

    # Runtime reuses stock PSOs for non-forward stages. None may call an edited pixel shader.
    for technique_name, pixel_shaders in named_by.items():
        if technique_name not in twin.forward and pixel_shaders.intersection(wanted):
            raise BuildError("%s: non-forward technique %s uses an edited pixel shader"
                             % (file_name, technique_name))

    calls: list[tuple[int, int]] = []
    for insert in twin.inserts:
        anchor, _ = cfxb.tokenize(insert.anchor)
        call, _ = cfxb.tokenize(insert.call)
        prelude = cfxb.tokenize(insert.prelude)[0] if insert.prelude else []
        at = find_unique(tokens, anchor + call, file_name, "the %s call after its anchor" % insert.ps) + len(anchor)
        calls.append((at, len(call)))
        # Every place the name is followed by "(" and reaches a "{" before any ";" or "}" is a signature; all of them
        # (one per #if branch) must open the same body.
        t_shader = word_token(insert.ps.encode("latin-1"))
        bodies = set()
        for i in range(len(tokens) - 1):
            if tokens[i] != t_shader or tokens[i + 1] != t_open_paren:
                continue
            j = i + 2
            while j < len(tokens) and tokens[j] not in (t_open_brace, t_close_brace, t_semicolon):
                j += 1
            if j < len(tokens) and tokens[j] == t_open_brace:
                bodies.add(j)
        if len(bodies) != 1:
            raise BuildError("%s: %s has %d bodies, expected 1" % (file_name, insert.ps, len(bodies)))
        body = bodies.pop()
        if tokens[body + 1 : at] != prelude:
            raise BuildError("%s: %s does not start with the dissolve%s"
                             % (file_name, insert.ps, " after its prelude" if prelude else ""))
        elsewhere = sorted(n for n, (own, _) in files.items() if n != file_name.lower() and t_shader in own)
        if elsewhere:
            raise BuildError("%s: %s is also named in %s" % (file_name, insert.ps, ", ".join(elsewhere)))
    for call_text in sorted({insert.call for insert in twin.inserts}):
        call, _ = cfxb.tokenize(call_text)
        made = len(find_all(tokens, call))
        expected = sum(1 for insert in twin.inserts if insert.call == call_text)
        if made != expected:
            raise BuildError("%s: makes the call %s %d time(s), expected %d"
                             % (file_name, call_text.decode(), made, expected))
    stripped = list(tokens)
    for at, length in sorted(calls, reverse=True):
        del stripped[at : at + length]
    if stripped[: len(salt)] != salt or stripped[len(salt) :] != stock_tokens:
        raise BuildError("%s: differs from its stock file beyond the dissolve calls%s"
                         % (file_name, " and the salt macro" if salt else ""))
    return ("%s (%s): %d include files, none of the mod's and no hole; the pixel shaders of %s (%s) each start with "
            "the dissolve, and the rest is stock token for token (techniques %s)"
            % (file_name, twin.label, len(files) - 1, ", ".join(twin.forward), ", ".join(named),
               ", ".join(sorted(named_by))))


def include_tree(file_name: str, tokens: list[int], table: dict[int, bytes],
                 resolver: PakResolver) -> dict[str, tuple[list[int], dict[int, bytes]]]:
    """A twin's whole include tree by lower-case name, each file once, the twin itself under file_name."""
    files: dict[str, tuple[list[int], dict[int, bytes]]] = {file_name.lower(): (tokens, table)}
    queue = [file_name.lower()]
    while queue:
        own_tokens, own_table = files[queue.pop(0)]
        for i, tok in enumerate(own_tokens[:-1]):
            if tok != cfxb.T_INCLUDE:
                continue
            name = cfxb.token_text(own_tokens[i + 1], own_table).lower()
            if name in files:
                continue
            loaded = resolver.load(name)
            if loaded is None:
                raise BuildError("%s: the include %s did not resolve" % (file_name, name))
            files[name] = loaded
            queue.append(name)
    return files


def top_level_definitions(tokens: list[int], name: int) -> list[int]:
    """Where one file defines name outside any braces: name right before one of ( = : ; { [, or a '#define' of it
    (the position of the '#define'; its line ends at a 0 token)."""
    delimiters = {word_token(text) for text in (b"(", b"=", b":", b";", b"{", b"[")}
    t_open_brace = word_token(b"{")
    t_close_brace = word_token(b"}")
    found = []
    depth = 0
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        if tok in (cfxb.T_DEFINE, cfxb.T_DEFINE_2):
            if depth == 0 and i + 1 < len(tokens) and tokens[i + 1] == name:
                found.append(i)
            i += 2
            while i < len(tokens) and tokens[i] != 0:
                i += 1
            i += 1
            continue
        if tok == t_open_brace:
            depth += 1
        elif tok == t_close_brace:
            depth -= 1
        elif depth == 0 and tok in delimiters and i > 0 and tokens[i - 1] == name:
            found.append(i - 1)
        i += 1
    return found


def world_view_pos_block(trees: list[tuple[str, list[int], dict[int, bytes]]], resolver: PakResolver) -> list[int]:
    """GetWorldViewPos's fragment, its return type through its closing brace, as a hole twin's Z-pass pixel shader
    carries it when the stock one does not. Each hole twin's include tree must define the function once, the same."""
    t_name = word_token(SPLICE_WORLD_VIEW_POS)
    t_open_brace = word_token(b"{")
    t_close_brace = word_token(b"}")
    blocks = set()
    for file_name, tokens, table in trees:
        files = include_tree(file_name, tokens, table, resolver)
        found = [(name, at) for name, (own, _) in files.items() for at in top_level_definitions(own, t_name)]
        if len(found) != 1 or found[0][1] == 0 or files[found[0][0]][0][found[0][1]] != t_name:
            raise BuildError("%s: %s has %d top-level definition(s) in its include tree, expected one function"
                             % (file_name, SPLICE_WORLD_VIEW_POS.decode(), len(found)))
        name, at = found[0]
        own = files[name][0]
        depth = 0
        end = at
        while True:
            if end >= len(own):
                raise BuildError("%s: %s in %s has no body" % (file_name, SPLICE_WORLD_VIEW_POS.decode(), name))
            if own[end] == t_open_brace:
                depth += 1
            elif own[end] == t_close_brace:
                depth -= 1
                if depth == 0:
                    break
            end += 1
        blocks.add(tuple(own[at - 1 : end + 1]))
    if len(blocks) != 1:
        raise BuildError("the hole twins' %s fragments differ" % SPLICE_WORLD_VIEW_POS.decode())
    return list(blocks.pop())


def shdata_hole(tokens: list[int]) -> list[int]:
    """The hole include's tokens as a twin's pixel-shader SHData carries them: each first-pass '#if [!]NAME' between
    '#skip_(' and '#skip_)', '#skip' in front of each '#endif'. frame_hole() already limits the hole to that form; any
    other directive is refused here too."""
    first_pass_if = (cfxb.T_IF, cfxb.T_IFDEF, cfxb.T_IFNDEF, cfxb.T_ELIF)
    t_not = cfxb.KEYWORD_ID[b"!"]
    t_endif = cfxb.KEYWORD_ID[b"#endif"]
    t_skip_open = cfxb.KEYWORD_ID[b"#skip_("]
    t_skip_close = cfxb.KEYWORD_ID[b"#skip_)"]
    directives = {tok for tok, text in enumerate(cfxb.KEYWORDS) if text and text.startswith("#")}
    out: list[int] = []
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        if tok in first_pass_if:
            end = i + 1
            if end < len(tokens) and tokens[end] == t_not:
                end += 1
            if end >= len(tokens) or tokens[end] < cfxb.KEYWORD_COUNT:
                raise BuildError("the hole's %s at token %d is not '[!]NAME'" % (cfxb.KEYWORDS[tok], i))
            out += [t_skip_open] + tokens[i : end + 1] + [t_skip_close]
            i = end + 1
            continue
        if tok == t_endif:
            out += [cfxb.T_SKIP, t_endif]
        elif tok in directives:
            raise BuildError("the hole has a %s, which the twin clone does not carry" % cfxb.KEYWORDS[tok])
        else:
            out.append(tok)
        i += 1
    return out


def splice_table(helper_name: str, helper_table: dict[int, bytes], pass_names: dict[str, str],
                 salt_table: dict[int, bytes]) -> dict[int, bytes]:
    """The strings a hole twin's string table has and the stock one's lacks: the names of its includes (the two pass
    copies and the hole), the hole's own strings and the salt macro's (--name-salt)."""
    add: dict[int, bytes] = {}
    for name in (pass_names[NAME_ZPREPASS], pass_names[NAME_ZPASS], helper_name):
        add.update(cfxb.tokenize(b'#include "%s.cfi"\n' % name.encode("latin-1"))[1])
    for strings in (helper_table, salt_table):
        for tok, text in strings.items():
            if add.setdefault(tok, text) != text:
                raise BuildError("the string id 0x%08X names both %r and %r" % (tok, add[tok], text))
    if any(include_tokens(stock)[1] in add for stock in SPLICE_TABLE_REMOVED):
        raise BuildError("a twin's string table would keep a stock pass include name")
    return add


def c_string(text: bytes) -> str:
    out = []
    for byte in text:
        if byte in b'"\\':
            out.append("\\" + chr(byte))
        elif 0x20 <= byte < 0x7F:
            out.append(chr(byte))
        else:
            out.append("\\%03o" % byte)
    return '"%s"' % "".join(out)


def token_literals(tokens: list[int]) -> list[str]:
    """Keyword ids in decimal, string CRCs in hex."""
    return ["%d" % tok if tok < cfxb.KEYWORD_COUNT else "0x%08X" % tok for tok in tokens]


def token_array(name: str, tokens: list[int], per_line: int = 10) -> list[str]:
    literals = token_literals(tokens)
    lines = ["    inline constexpr std::uint32_t %s[] = {" % name]
    for i in range(0, len(literals), per_line):
        lines.append("        " + ", ".join(literals[i : i + per_line]) + ",")
    return lines + ["    };"]


def splice_lines(holes: dict[str, tuple[str, list[int], dict[int, bytes]]], kinds: list[tuple[str, str]],
                 world_view_pos: list[int]) -> list[str]:
    """The C++ constants of the DEV twin clone. holes maps a hole key to its include name, SHData tokens and table
    additions; kinds names each hole twin's label and hole key."""
    def word(text: bytes) -> str:
        return token_literals([word_token(text)])[0]

    def run(text: bytes) -> str:
        return ", ".join(token_literals(cfxb.tokenize(text)[0]))

    lines = [
        "",
        "    // The DEV twin clone (hooks/shader_token_splice.hpp): what it needs to derive a hole twin's Z-pass pixel",
        "    // shaders (their SHData token stream and string table) from the stock ones' without parsing the twin."
        " Token",
        "    // ids are the game tokenizer's: a keyword id below k_splice_keyword_count, else the CRC-32 of the text.",
        "    inline constexpr std::uint32_t k_splice_keyword_count = %d;" % cfxb.KEYWORD_COUNT,
        "    // '#define' (both forms, each line ends at a 0 token), the braces and ';' that end a top-level fragment,"
        " and",
        "    // the tokens whose preceding identifier names a fragment: ( = : ; { [",
        "    inline constexpr std::uint32_t k_splice_define[] = {%d, %d};" % (cfxb.T_DEFINE, cfxb.T_DEFINE_2),
        "    inline constexpr std::uint32_t k_splice_open_brace = %s;" % word(b"{"),
        "    inline constexpr std::uint32_t k_splice_close_brace = %s;" % word(b"}"),
        "    inline constexpr std::uint32_t k_splice_semicolon = %s;" % word(b";"),
        "    inline constexpr std::uint32_t k_splice_name_delimiters[] = {%s};"
        % ", ".join(word(text) for text in (b"(", b"=", b":", b";", b"{", b"[")),
        "    // %s, which the pass copies delete in front of their pixel shaders." % EARLY_DEPTH.decode(),
        "    inline constexpr std::uint32_t k_splice_early_depth = %d;" % cfxb.KEYWORD_ID[EARLY_DEPTH],
        "    // '%s' and '%s'" % (ZPREPASS_PS.decode(), ZPASS_PS.decode()),
        "    inline constexpr std::uint32_t k_splice_zprepass_signature[] = {%s};" % run(ZPREPASS_PS),
        "    inline constexpr std::uint32_t k_splice_zpass_signature[] = {%s};" % run(ZPASS_PS),
        "    // %s, which the copies add after the dissolve call (k_splice_dissolve_call)" % HOLE_CALL.decode(),
        "    inline constexpr std::uint32_t k_splice_hole_call[] = {%s};" % run(HOLE_CALL),
        "    // %s: the fragment the hole follows (ShadeLib's last function)" % SPLICE_HOLE_AFTER.decode(),
        "    inline constexpr std::uint32_t k_splice_hole_after = %s;" % word(SPLICE_HOLE_AFTER),
        "    // %s: the fragments the hole needs emitted" % ", ".join(text.decode() for text in SPLICE_REQUIRED),
        "    inline constexpr std::uint32_t k_splice_required[] = {%s};"
        % ", ".join(word(text) for text in SPLICE_REQUIRED),
        "    // %s, and the fragment it follows in each pass (%s, %s) when the stock"
        % (SPLICE_WORLD_VIEW_POS.decode(), SPLICE_ZPREPASS_WORLD_VIEW_POS_AFTER.decode(),
           SPLICE_ZPASS_WORLD_VIEW_POS_AFTER.decode()),
        "    // stream does not emit it",
        "    inline constexpr std::uint32_t k_splice_world_view_pos = %s;" % word(SPLICE_WORLD_VIEW_POS),
        "    inline constexpr std::uint32_t k_splice_zprepass_world_view_pos_after = %s;"
        % word(SPLICE_ZPREPASS_WORLD_VIEW_POS_AFTER),
        "    inline constexpr std::uint32_t k_splice_zpass_world_view_pos_after = %s;"
        % word(SPLICE_ZPASS_WORLD_VIEW_POS_AFTER),
        "    // The stock pass include names a twin's string table drops: %s" % ", ".join(SPLICE_TABLE_REMOVED),
        "    inline constexpr std::uint32_t k_splice_table_removed[] = {%s};"
        % ", ".join(token_literals([include_tokens(stock)[1] for stock in SPLICE_TABLE_REMOVED])),
        "",
        "    /** @brief A string a twin's table adds. */",
        "    struct SpliceString",
        "    {",
        "        std::uint32_t id;",
        "        const char *text;",
        "    };",
        "",
        "    /** @brief A hole twin kind the DEV clone can derive (the other kinds are parsed). */",
        "    struct CloneSplice",
        "    {",
        "        const char *label;                       // the k_twins label",
        "        std::span<const std::uint32_t> hole;     // the hole include's tokens as the twin's SHData has them",
        "        std::span<const SpliceString> table_add; // the strings the twin's table adds, sorted by id",
        "    };",
        "",
        "    // clang-format off",
        "    // %s" % DISSOLVE_CALL.decode(),
    ]
    lines += token_array("k_splice_dissolve_call", cfxb.tokenize(DISSOLVE_CALL)[0])
    lines.append("    // Common.cfi's GetWorldViewPos fragment: %d tokens" % len(world_view_pos))
    lines += token_array("k_splice_world_view_pos_block", world_view_pos)
    for key, (name, tokens, _) in holes.items():
        lines.append("    // %s.cfib as SHData has it (%d tokens)" % (name.lower(), len(tokens)))
        lines += token_array("k_splice_hole_%s" % key, tokens)
    for key, (name, _, table) in holes.items():
        lines.append("    // %d strings: the include names of the two pass copies and of %s, and its own strings"
                     % (len(table), name))
        lines.append("    inline constexpr SpliceString k_splice_table_%s[] = {" % key)
        lines += ["        {0x%08X, %s}," % (tok, c_string(text)) for tok, text in sorted(table.items())]
        lines.append("    };")
    lines += [
        "    // clang-format on",
        "",
        "    inline constexpr CloneSplice k_clone_splices[] = {",
    ]
    lines += ['        {"%s", k_splice_hole_%s, k_splice_table_%s},' % (label, key, key) for label, key in kinds]
    lines.append("    };")
    return lines


def serialize(tokens: list[int], table: dict[int, bytes], crc: int, source_crc: int) -> bytes:
    data = cfxb.serialize(tokens, table, crc, source_crc)
    back = cfxb.parse(data)
    if back["tokens"] != tokens or back["strings"] != table or back["header"]["crc"] != crc:
        raise BuildError("serialized file does not parse back to its tokens")
    return data


def write_header(path: str, blobs: list[tuple[str, bytes, str]],
                 twins: list[tuple[str, str, str, str, int, str, bool]], sources: dict[str, int], splice: list[str],
                 name_salt: str = "") -> None:
    src_dir = os.path.join(REPO_DIR, "src")
    try:
        rel = os.path.relpath(path, src_dir).replace("\\", "/")
    except ValueError:  # another drive
        rel = ".."
    if rel.startswith(".."):
        rel = os.path.basename(path)
    guard = "TPVCAMERA_" + "".join(c if c.isalnum() else "_" for c in rel.upper())
    lines = [
        "/**",
        " * @file %s" % rel,
        " * @brief GENERATED by scripts/build_shader_twins.py from the game's Engine/ShadersBin.pak and",
        " *        shaders/TPVCameraHole.cfi; do not edit.",
        " *",
        " * TPVCamera's shader twins, as tokenized shader binaries for the user shader cache. The tree see-through",
        " * twins of the stock Vegetation and Illum shaders: the hole function, copies of CommonZPrePass and",
        " * CommonZPass that call it after their ApplyDissolve (with late depth for their pixel shaders), and a copy",
        " * of each source shader that includes the hole after ShadeLib and uses the two pass copies. The bare frames'",
        " * twin of the stock Illum shader: another copy, whose hole include keeps only the near cut. The character",
        " * fade twins of the stock Eye, Hair and Illum shaders: copies whose forward pixel shaders apply the",
        " * dissolve, with the stock includes. Each name ends with the file's own header CRC.",
    ]
    if name_salt:
        lines += [
            " *",
            " * A first-run test build (--name-salt %s): every twin starts with an unused macro," % name_salt,
            " * so every twin's name is new.",
        ]
    lines += [
        " */",
        "#ifndef %s" % guard,
        "#define %s" % guard,
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <span>",
        "",
        "namespace TPVCamera::shader_twin_blobs",
        "{",
        "    struct Blob",
        "    {",
        "        const char *file_name; // lower-case file name for the user shader cache dir",
        "        const unsigned char *data;",
        "        std::size_t size;",
        "    };",
        "",
        "    /** @brief Which items draw with a twin. */",
        "    enum class TwinScope : std::uint8_t",
        "    {",
        "        ListedPlants, // the main-view items of the plants the camera lists (the window and the near cut)",
        "        ListedFrames, // the main-view items of the bare frames the camera lists (the near cut)",
        "        Dissolving,   // the main-view items of a dissolving temporary render object of the character",
        "    };",
        "",
        "    /** @brief A stock shader the mod copies, and its twin. A source name has one twin per scope at most. */",
        "    struct Twin",
        "    {",
        "        const char *label;        // the kind's name, for logs",
        "        const char *source_name;  // the stock shader's name, whose .ext the twin's gen flags come from",
        "        const char *twin_name;    // the twin's shader name for CShaderMan::mfForName",
        "        const char *source_file;  // the stock binary in ShadersBin.pak the twin was spliced from",
        "        std::uint32_t source_crc; // its header CRC, which covers all of its includes",
        "        TwinScope scope;          // which items swap to it",
        "        bool transparent_only;    // only an item whose opacity is below 1 swaps to it",
        "    };",
        "",
        "    // Every file the DLL writes starts with this; files with this prefix and another name are stale and"
        " deleted.",
        '    inline constexpr char k_file_prefix[] = "%s";' % FILE_PREFIX,
        "    // The DLL compares each source CRC (and the two below, of the pass includes the plant and frame twins'"
        " copies",
        "    // were spliced from) with the live ShadersBin.pak copies: a changed source turns its twin off, a changed"
        " pass",
        "    // include the plant and frame twins.",
        "    inline constexpr Twin k_twins[] = {",
    ]
    for label, source_name, twin_name, source_file, source_crc, scope, transparent_only in twins:
        head = '        {"%s", "%s", "%s", "%s", 0x%08X,' % (label, source_name, twin_name, source_file, source_crc)
        tail = ["TwinScope::%s," % scope, "%s}," % ("true" if transparent_only else "false")]
        # Packed as clang-format packs a braced list at 120 columns: each element on the last line it still fits on.
        row = [head]
        for element in tail:
            if len(row[-1]) + 1 + len(element) <= 120:
                row[-1] += " " + element
            else:
                row.append("         " + element)
        lines += row
    lines += [
        "    };",
        "    inline constexpr std::uint32_t k_source_commonzpass_crc = 0x%08X;" % sources["commonzpass.cfib"],
        "    inline constexpr std::uint32_t k_source_commonzprepass_crc = 0x%08X;" % sources["commonzprepass.cfib"],
        "",
        "    // clang-format off",
    ]
    for index, (file_name, data, summary) in enumerate(blobs):
        lines.append("    // %s: %s" % (file_name, summary))
        lines.append("    inline constexpr unsigned char k_blob%d[] = {" % index)
        for i in range(0, len(data), 16):
            lines.append("        " + ", ".join("0x%02X" % b for b in data[i : i + 16]) + ",")
        lines.append("    };")
    lines.append("    // clang-format on")
    lines.append("")
    lines.append("    // Includes first, the shaders last.")
    lines.append("    inline constexpr Blob k_blobs[] = {")
    for index, (file_name, _, _) in enumerate(blobs):
        lines.append('        {"%s", k_blob%d, sizeof(k_blob%d)},' % (file_name, index, index))
    lines.append("    };")
    lines += splice
    lines += ["} // namespace TPVCamera::shader_twin_blobs", "", "#endif // %s" % guard, ""]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))


def build(pak_path: str, header_path: str, out_dir: str | None, name_salt: str = "") -> int:
    salt, salt_table = salt_define(name_salt)
    binaries = read_pak(pak_path)
    resolver = PakResolver(binaries)
    parsed = check_stock(binaries, resolver)
    stock_files = list(dict.fromkeys([twin.source_file for twin in TWINS] + ["commonzpass.cfib",
                                                                             "commonzprepass.cfib"]))
    for need in stock_files:
        if need not in parsed:
            raise BuildError("%s is not in %s" % (need, pak_path))
    sources = {n: parsed[n]["header"]["crc"] for n in stock_files}

    # The hole function, the only part tokenized from source. Line endings are normalized for its source CRC so the
    # output does not depend on how git checked the file out.
    with open(HOLE_SOURCE, "rb") as fh:
        hole_source = fh.read()
    hole_tokens, hole_table = cfxb.tokenize(hole_source)
    if cfxb.T_INCLUDE in hole_tokens:
        raise BuildError("%s must not #include anything" % HOLE_SOURCE)
    for symbol in (b"TPVApplyHole", b"TPVHoleAmount", FRAME_MACRO):
        if zlib.crc32(symbol) & 0xFFFFFFFF not in hole_table:
            raise BuildError("%s does not name %s" % (HOLE_SOURCE, symbol.decode()))
    hole_crc = cfxb.compute_crc(hole_tokens, hole_table, resolver, (NAME_HOLE.lower(),))
    hole_name = "%s_%08x" % (NAME_HOLE, hole_crc)
    resolver.add(hole_name, hole_tokens, hole_table)
    hole_bin = serialize(hole_tokens, hole_table, hole_crc, cfxb.crc32(hole_source.replace(b"\r\n", b"\n")))

    built = [(hole_name, ".cfib", hole_bin, len(hole_tokens), hole_crc)]
    near_tokens, near_table = frame_hole(hole_tokens, hole_table)
    near_crc = cfxb.compute_crc(near_tokens, near_table, resolver, (NAME_NEAR_HOLE.lower(),))
    near_name = "%s_%08x" % (NAME_NEAR_HOLE, near_crc)
    resolver.add(near_name, near_tokens, near_table)
    built.append((near_name, ".cfib", serialize(near_tokens, near_table, near_crc, 0), len(near_tokens), near_crc))
    edits: list[str] = []
    pass_names = {}
    for stock_file, base, signature in (("commonzprepass.cfib", NAME_ZPREPASS, ZPREPASS_PS),
                                        ("commonzpass.cfib", NAME_ZPASS, ZPASS_PS)):
        tokens, table = splice_pass(parsed[stock_file], stock_file, signature, edits)
        crc = cfxb.compute_crc(tokens, table, resolver, (base.lower(),))
        name = "%s_%08x" % (base, crc)
        resolver.add(name, tokens, table)
        built.append((name, ".cfib", serialize(tokens, table, crc, 0), len(tokens), crc))
        pass_names[base] = name

    twins: list[tuple[str, str, str, str, int, str, bool]] = []
    coverage: list[str] = []
    hole_trees: list[tuple[str, list[int], dict[int, bytes]]] = []
    clone_kinds: list[tuple[str, str]] = []
    for twin in TWINS:
        source_file = twin.source_file
        if twin.scope in SCOPES_HOLE:
            helper = near_name if twin.scope == SCOPE_FRAMES else hole_name
            tokens, table = splice_twin(parsed[source_file], source_file, helper, pass_names[NAME_ZPREPASS],
                                        pass_names[NAME_ZPASS], edits)
        else:
            tokens, table = splice_forward(parsed[source_file], "%s (%s)" % (source_file, twin.label), twin.inserts,
                                           edits)
        if salt:
            tokens = salt + tokens
            table = {**table, **salt_table}
            edits.append("  %s (%s): salt macro TPVCAMERA_NAME_SALT_%s at the top" % (source_file, twin.label,
                                                                                    name_salt))
        resolver.missing.clear()
        crc = cfxb.compute_crc(tokens, table, resolver)
        if resolver.missing:
            raise BuildError("the %s twin's includes did not all resolve: %s"
                             % (twin.label, ", ".join(sorted(resolver.missing))))
        if twin.scope in SCOPES_HOLE:
            coverage.append(check_coverage(source_file, tokens, table, resolver,
                                           {"ZPass": pass_names[NAME_ZPASS], "ZPrePass": pass_names[NAME_ZPREPASS]}))
            hole_trees.append((source_file, tokens, table))
            clone_kinds.append((twin.label, "frame" if twin.scope == SCOPE_FRAMES else "full"))
        else:
            coverage.append(check_forward_coverage(source_file, tokens, table, resolver, twin,
                                                   parsed[source_file]["tokens"], salt))
        twin_name = "%s%s_%08x" % (NAME_PREFIX, twin.label, crc)
        # No engine param cache: it indexes the stock token stream's code fragments, which changed.
        built.append((twin_name, ".cfxb", serialize(tokens, table, crc, 0), len(tokens), crc))
        twins.append((twin.label, twin.source_name, twin_name, source_file, sources[source_file], twin.scope,
                      twin.transparent_only))

    holes = {
        "full": (hole_name, shdata_hole(hole_tokens), splice_table(hole_name, hole_table, pass_names, salt_table)),
        "frame": (near_name, shdata_hole(near_tokens), splice_table(near_name, near_table, pass_names, salt_table)),
    }
    world_view_pos = world_view_pos_block(hole_trees, resolver)
    splice = splice_lines(holes, clone_kinds, world_view_pos)

    print("stock: " + ", ".join("%s 0x%08X" % (n, sources[n]) for n in stock_files))
    print("edits:")
    print("\n".join(edits))
    print("coverage:")
    print("\n".join("  " + line for line in coverage))
    blobs = []
    for name, ext, data, ntok, crc in built:
        file_name = (name + ext).lower()
        if not file_name.startswith(FILE_PREFIX):
            raise BuildError("%s does not start with %s" % (file_name, FILE_PREFIX))
        summary = "%d bytes, %d tokens, crc 0x%08X" % (len(data), ntok, crc)
        print("built %s: %s" % (file_name, summary))
        blobs.append((file_name, data, summary))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        for file_name, data, _ in blobs:
            with open(os.path.join(out_dir, file_name), "wb") as fh:
                fh.write(data)
    print("clone splice: hole %d and %d tokens, GetWorldViewPos %d, table additions %d and %d, kinds %s"
          % (len(holes["full"][1]), len(holes["frame"][1]), len(world_view_pos), len(holes["full"][2]),
             len(holes["frame"][2]), ", ".join(label for label, _ in clone_kinds)))
    write_header(header_path, blobs, twins, sources, splice, name_salt)
    print("shaders %s -> %s" % (", ".join(row[2] for row in twins), header_path))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pak", default=DEFAULT_PAK, help="the game's Engine/ShadersBin.pak (default: %(default)s)")
    ap.add_argument("--header", default=DEFAULT_HEADER, help="C++ header to write (default: %(default)s)")
    ap.add_argument("--out-dir", help="also write the built binaries here, for inspection with cfxb.py dump")
    ap.add_argument("--name-salt", default="", metavar="TEXT",
                    help="put an unused macro holding TEXT at the top of every twin, so every twin gets a new name and "
                         "compiles from scratch (to test a first run); default: none")
    args = ap.parse_args(argv)
    try:
        return build(args.pak, args.header, args.out_dir, args.name_salt)
    except BuildError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
