"""Build TPVCamera's shader twins (the close-up fade's eyes, hair and eye film) and embed them as a C++ header.

The close-up fade dithers the character out, but three stock shaders draw parts of him again in forward passes whose
pixel shaders never call ApplyDissolve (only their G-buffer and Z passes do): Eye (the eyes, and the eye's AO
overlay), Hair (the eyelashes, the beard, the hair and a hood's hair cards, alpha-blended after the G-buffer) and
Illum on a transparent item (the wet film over the eyes). The retail game only loads pre-tokenized shaders (see
cfxb.py), so each twin is a copy of the game's own binary spliced at the token level, nothing re-tokenized from
source. A twin keeps the stock includes and adds the dissolve test to its forward pixel shaders, nothing else:
ApplyDissolve's own test on Get_SPI_Dissolve(), but not behind %_RT_DISSOLVE, which the engine takes out of the
forward pass of the items its Z prepass drew. The per-draw dissolve is 0 while nothing dissolves, so a twin then draws
like the stock shader:

    tpvcamera_eye_<crc>.cfxb            eye.cfxb, the call (on IN.WPos) as the first statement of EyePS and
                                        EyeOverlayPS
    tpvcamera_hair_<crc>.cfxb           hair.cfxb, the call (on IN.Common.WPos, vert2FragHair's vert2FragGeneral) as
                                        the first statement of HairPS (General's back pass, HairFrontPass and
                                        HairBackPass) and HairOpaquePS (General's opaque pass)
    tpvcamera_illumfade_<crc>.cfxb      illum.cfxb, the call (on IN.WPos) in IlluminationPS right after its parallax
                                        occlusion fix-up (#if %SILHOUETTE_PARALLAX_OCCLUSION_MAPPING ... #endif),
                                        where IN exists with either signature; the DLL swaps it only on a transparent
                                        item (opacity below 1)

The shader names are TPVCamera_<kind>_<crc> (Eye, Hair, IllumFade); the DLL writes the files to the user shader cache
and creates a twin with CShaderMan::mfForName for each source instance it swaps on the character. Every <crc> is the
file's own header CRC, so any change to the game's shaders gives new names (the engine keeps a loaded shader by name
for the session).

Header CRC (the engine's sub_180918A8C, checked on every load): CRC-32 of the token array plus, for every #include
token, the same value computed for the included .cfib, recursively. The script proves its routine first: the
computed CRC of every binary in the pak must equal the one stored in its header.

Every splice point is found by its token pattern and must occur exactly once, else the script stops with an error and
writes nothing. Each twin is then checked: every pixel shader its forward techniques name starts with the dissolve,
and nothing else in the twin differs from its stock file. The header CRCs of the stock files go into the C++ header,
so the DLL can turn a twin off when the game's shaders change.

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
from typing import NamedTuple

sys.dont_write_bytecode = True  # no __pycache__ next to the scripts
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cfxb  # noqa: E402

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PAK = "D:/Games/SteamLibrary/steamapps/common/KingdomComeDeliverance2/Engine/ShadersBin.pak"
DEFAULT_HEADER = os.path.join(REPO_DIR, "src", "generated", "shader_twin_blobs.hpp")
PAK_SHADER_DIR = "shaders/cache/d3d12/"

FILE_PREFIX = "tpvcamera_"

# Token patterns, tokenized here with the game's rules so they match the shipped binaries exactly.
# The twins' dissolve: the stock ApplyDissolve's test (TestDissolve: the 4x4 threshold against the per-draw dissolve,
# its sign picking in or out), but not behind %_RT_DISSOLVE. The forward pass of the items the Z prepass drew
# (sub_1807C5A20's pass 1, 0x1807C5B50 and 0x1807C5E41) takes the dissolve bit out of the runtime mask it compiles, so
# ApplyDissolve would compile to nothing there and the eyes and the eye film would stay solid over the faded
# character. Get_SPI_Dissolve() reads the per-draw constant every pass sees; it is 0 while nothing dissolves, and a 0
# clips nothing (the threshold is never below 0).
FADE_CALL = (b"{ const float tpvFade = Get_SPI_Dissolve(); clip((tpvFade >= 0 ? 1.0 : -1.0) * "
                 b"(GetDissolveThreshold17((int2)IN.WPos.xy) - abs(tpvFade))); }")
# The same where the pixel shader's input is vert2FragHair, whose vert2FragGeneral is its member Common. Hair's
# General pass fills IN.Common.baseTC from HairVS, so the dissolve its own Z passes read from baseTC.z is not there.
FADE_CALL_HAIR = (b"{ const float tpvFade = Get_SPI_Dissolve(); clip((tpvFade >= 0 ? 1.0 : -1.0) * "
                      b"(GetDissolveThreshold17((int2)IN.Common.WPos.xy) - abs(tpvFade))); }")


class Insert(NamedTuple):
    """One dissolve call a twin adds, right after anchor (which must occur exactly once in the stock file): the
    first statement of the pixel shader ps, or the first after prelude when its body opens with that."""

    anchor: bytes
    call: bytes
    ps: str
    prelude: bytes = b""


class Twin(NamedTuple):
    """A stock shader that gets a twin. label names the kind (the twin's name is TPVCamera_<label>_<crc>);
    source_name is the name the engine knows the stock shader by (and loads its .ext with), source_file its binary in
    the pak. The inserts must cover every pixel shader the techniques in forward name; transparent_only tells the DLL
    to swap it only on an item whose opacity is below 1 (the engine's own test for the transparent list)."""

    label: str
    source_name: str
    source_file: str
    forward: tuple[str, ...]
    inserts: tuple[Insert, ...]
    transparent_only: bool = False


# Eye.cfx and Hair.cfx define HIDING_GROUPS, which drops the early-depth attribute in front of their pixel shaders, and
# the engine expands IlluminationPS's CRY_EARLY_DEPTH to nothing in a permutation with the dissolve, so a clip() keeps
# its meaning in each. Hair's chained HairFrontPass and HairBackPass (the multi-pass hair of the beard and the hood)
# name HairPS too; ThinHairDepthPS is named by no technique. IlluminationPS takes geom2FragGeneral IN_ext under
# %SILHOUETTE_PARALLAX_OCCLUSION_MAPPING and copies its IN out first; the call goes after that copy, so IN exists with
# either signature.
TWINS = (
    Twin("Eye", "Eye", "eye.cfxb", forward=("General",), inserts=(
        Insert(b"pixout EyePS(vert2FragGeneral IN)\n{", FADE_CALL, "EyePS"),
        Insert(b"min16float2 EyeOverlayPS(vert2FragGeneral IN) : SV_Target0\n{", FADE_CALL, "EyeOverlayPS"),
    )),
    Twin("Hair", "Hair", "hair.cfxb", forward=("General", "HairFrontPass", "HairBackPass"), inserts=(
        Insert(b"pixoutHair2 HairPS(vert2FragHair IN)\n{", FADE_CALL_HAIR, "HairPS"),
        Insert(b"pixoutHair HairOpaquePS(vert2FragHair IN)\n{", FADE_CALL_HAIR, "HairOpaquePS"),
    )),
    Twin("IllumFade", "Illum", "illum.cfxb", forward=("General",), transparent_only=True, inserts=(
        Insert(b"vert2FragGeneral IN = IN_ext.IN;\n#endif", FADE_CALL, "IlluminationPS",
               prelude=b"#if %SILHOUETTE_PARALLAX_OCCLUSION_MAPPING\nvert2FragGeneral IN = IN_ext.IN;\n#endif"),
    )),
)


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
    """Resolves #include names like the engine's GetBinShader(name, bInclude=true): the .cfib of that name, from the
    pak (read into memory)."""

    def __init__(self, binaries: dict[str, bytes]):
        super().__init__(())
        self.binaries = binaries

    def load(self, name: str):
        key = name.lower()
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


def splice_forward(sb: dict, file_name: str, inserts: tuple[Insert, ...],
                   log: list[str]) -> tuple[list[int], dict[int, bytes]]:
    """A twin's copy: each insert's dissolve call right after its anchor. A stock file that already makes one of the
    calls is refused; other ApplyDissolve calls (Hair's own Z passes read baseTC.z) stay."""
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


def word_token(text: bytes) -> int:
    """The token a single word becomes (a keyword id or the CRC of its text)."""
    tokens, _ = cfxb.tokenize(text)
    if len(tokens) != 1:
        raise BuildError("'%s' is not a single token" % text.decode("latin-1"))
    return tokens[0]


def check_forward_coverage(file_name: str, tokens: list[int], table: dict[int, bytes], resolver: PakResolver,
                           twin: Twin, stock_tokens: list[int], salt: list[int]) -> str:
    """Proves on a twin's whole include tree that its forward pixel shaders dissolve and nothing else changed.

    Every technique in twin.forward must exist in the twin's own stream, and the pixel shaders they name
    (PixelShader = <name>(...)) must be exactly the inserts'. Each of those has one body, in the twin, is named by no
    include, and its insert's call opens that body (after the insert's prelude, if any). Taken out the calls and the
    salt macro at the top (--name-salt), the twin is its stock file token for token, so every other technique (its Z
    passes, shadow, motion blur and custom passes, and the includes') draws like the stock shader. Returns a summary
    for the log.
    """
    t_technique = word_token(b"technique")
    t_pixel_shader = word_token(b"PixelShader")
    t_equals = word_token(b"=")
    t_open_paren = word_token(b"(")
    t_open_brace = word_token(b"{")
    t_close_brace = word_token(b"}")
    t_semicolon = word_token(b";")

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
    return ("%s (%s): %d include files; the pixel shaders of %s (%s) each start with the dissolve, and the rest is "
            "stock token for token (techniques %s)"
            % (file_name, twin.label, len(files) - 1, ", ".join(twin.forward), ", ".join(named),
               ", ".join(sorted(named_by))))


def serialize(tokens: list[int], table: dict[int, bytes], crc: int, source_crc: int) -> bytes:
    data = cfxb.serialize(tokens, table, crc, source_crc)
    back = cfxb.parse(data)
    if back["tokens"] != tokens or back["strings"] != table or back["header"]["crc"] != crc:
        raise BuildError("serialized file does not parse back to its tokens")
    return data


def write_header(path: str, blobs: list[tuple[str, bytes, str]], twins: list[tuple[str, str, str, str, int, bool]],
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
        " * @brief GENERATED by scripts/build_shader_twins.py from the game's Engine/ShadersBin.pak; do not edit.",
        " *",
        " * TPVCamera's shader twins, as tokenized shader binaries for the user shader cache: the close-up fade twins",
        " * of the stock Eye, Hair and Illum shaders, copies whose forward pixel shaders apply the dissolve, with the",
        " * stock includes. Each name ends with the file's own header CRC.",
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
        "    /** @brief A stock shader the mod copies, and its twin. A source name has one twin at most. */",
        "    struct Twin",
        "    {",
        "        const char *label;        // the kind's name, for logs",
        "        const char *source_name;  // the stock shader's name, whose .ext the twin's gen flags come from",
        "        const char *twin_name;    // the twin's shader name for CShaderMan::mfForName",
        "        const char *source_file;  // the stock binary in ShadersBin.pak the twin was spliced from",
        "        std::uint32_t source_crc; // its header CRC, which covers all of its includes",
        "        bool transparent_only;    // only an item whose opacity is below 1 swaps to it",
        "    };",
        "",
        "    // Every file the DLL writes starts with this; files with this prefix and another name are stale and"
        " deleted.",
        '    inline constexpr char k_file_prefix[] = "%s";' % FILE_PREFIX,
        "    // The DLL compares each source CRC with the live ShadersBin.pak copy: a changed source turns its twin off.",
        "    inline constexpr Twin k_twins[] = {",
    ]
    for label, source_name, twin_name, source_file, source_crc, transparent_only in twins:
        head = '        {"%s", "%s", "%s", "%s", 0x%08X,' % (label, source_name, twin_name, source_file, source_crc)
        tail = "%s}," % ("true" if transparent_only else "false")
        # Wrapped where clang-format would wrap it, at 120 columns.
        lines += [head + " " + tail] if len(head) + 1 + len(tail) <= 120 else [head, "         " + tail]
    lines += [
        "    };",
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
    lines.append("    inline constexpr Blob k_blobs[] = {")
    for index, (file_name, _, _) in enumerate(blobs):
        lines.append('        {"%s", k_blob%d, sizeof(k_blob%d)},' % (file_name, index, index))
    lines += ["    };", "} // namespace TPVCamera::shader_twin_blobs", "", "#endif // %s" % guard, ""]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))


def build(pak_path: str, header_path: str, out_dir: str | None, name_salt: str = "") -> int:
    salt, salt_table = salt_define(name_salt)
    binaries = read_pak(pak_path)
    resolver = PakResolver(binaries)
    parsed = check_stock(binaries, resolver)
    stock_files = list(dict.fromkeys(twin.source_file for twin in TWINS))
    for need in stock_files:
        if need not in parsed:
            raise BuildError("%s is not in %s" % (need, pak_path))
    sources = {n: parsed[n]["header"]["crc"] for n in stock_files}

    built: list[tuple[str, str, bytes, int, int]] = []
    edits: list[str] = []
    twins: list[tuple[str, str, str, str, int, bool]] = []
    coverage: list[str] = []
    for twin in TWINS:
        source_file = twin.source_file
        tokens, table = splice_forward(parsed[source_file], "%s (%s)" % (source_file, twin.label), twin.inserts, edits)
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
        coverage.append(check_forward_coverage(source_file, tokens, table, resolver, twin,
                                               parsed[source_file]["tokens"], salt))
        twin_name = "TPVCamera_%s_%08x" % (twin.label, crc)
        # No engine param cache: it indexes the stock token stream's code fragments, which changed.
        built.append((twin_name, ".cfxb", serialize(tokens, table, crc, 0), len(tokens), crc))
        twins.append((twin.label, twin.source_name, twin_name, source_file, sources[source_file],
                      twin.transparent_only))

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
    write_header(header_path, blobs, twins, name_salt)
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
