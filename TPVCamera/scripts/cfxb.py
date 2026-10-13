"""KCD2 CryFX binary shader tool: parse, build and verify .cfxb / .cfib files.

Kingdom Come: Deliverance II (retail, CryEngine 5 fork) only loads pre-tokenized shaders from
Shaders/Cache/D3D12/<name>.cfxb (techniques, from <Name>.cfx) and <name>.cfib (includes, from <Name>.cfi).
The default disk cache is %USERPROFILE%/Saved Games/kingdomcome2/shaders/cache/d3d12/.
The retail DLL has no tokenizer (CShaderManBin::SaveBinShader is compiled out), so a mod that ships its own
shader must ship the binary. This tool re-implements CE5's SaveBinShader tokenizer so that the output is
byte-identical to what the game ships.

File layout (little endian):

    +0x00  char[4]  magic               'FXB0'
    +0x04  u32      crc                 CRC of the token stream, see compute_crc() (verified at load time)
    +0x08  u16      version_low         3   (FX_CACHE_VER 4.3; CE5 open source is 1.0)
    +0x0A  u16      version_high        4
    +0x0C  u32      offset_string_table = 28 + 4 * token_count
    +0x10  u32      offset_local_info   = end of the string table
    +0x14  u32      token_count
    +0x18  u32      source_crc          zlib CRC-32 of the raw .cfx/.cfi bytes (not checked by retail)
    +0x1C  u32[token_count]             token stream
    ...    string table                 (u32 token, char[] zero-terminated), sorted by token, one entry per
                                        distinct non-keyword token (identifiers, numbers, include/macro names)
    ...    local info (optional, .cfxb only)
                                        engine-written parameter cache, appended by the game's own shader parse
                                        while the shipped cache was generated; repeated until EOF:
                                          u64 gen_mask, u32 name_crc, i32 n_params, i32 n_samplers,
                                          i32 n_textures, i32 n_funcs, 4 pad bytes (32-byte header),
                                          then i32 params[], samplers[], textures[], funcs[]

Token values below KEYWORD_COUNT are keyword ids (table KEYWORDS, recovered from the retail key-token
registration function sub_180E47F5C, g_KeyTokens @ 0x1850DA6A0); larger values are zlib CRC-32 of the token text.

Header CRC (compute_crc): CRC-32 of the token array bytes, plus (u32 addition) the same CRC recomputed for every
'#include' token, recursively, from the include's own tokens. The game recomputes it on every load
(sub_1809183EC -> sub_180918A8C) and REJECTS a .cfxb/.cfib whose stored value differs, so a file is tied to the
exact .cfib versions it includes.

Token stream encoding (identical to CE5 SaveBinShader; only the keyword ids and the version differ):
    #include "Foo.cfi"        -> 1, CRC("Foo")               (quotes and extension dropped, case kept)
    #define NAME body         -> 2, CRC(NAME), body tokens..., 0   (0 terminates the macro line)
    #define %_NAME body       -> 3 (eT_define_2), ...
    #if/#ifdef/#ifndef/#elif  -> 6/7/8/12, or 9/10/11/13 when the rest of the line has a word starting '%_'
                                 (a runtime/shader-gen flag, resolved in the engine's second preprocess pass)
    comments, whitespace, CR  -> dropped; no line information is kept
    "1.5f"                    -> '1' '.' '5f' (delimiters are ! " & ' ( ) * + , - . / : ; < = > ? [ ] { | })
    empty stream              -> single 23 (#skip)

KCD2 vs CE5 open source: FX_CACHE_VER 4.3 (header 3/4) instead of 1.0; 613 keywords instead of 610 with shifted
ids (e.g. STANDARDSGLOBAL 232 vs 239, Script 266 vs 273); load-time CRC check. Tokenizer rules are unchanged.

CLI:
    py -3 cfxb.py verify <srcdir> <bindir>      round-trip every source/binary pair
    py -3 cfxb.py build <in.cfx|in.cfi> <out>   tokenize one source file (-I <dir> for include lookup)
    py -3 cfxb.py dump <file.cfxb|file.cfib>    print header, tables and the detokenized stream
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys
import zlib

MAGIC = b"FXB0"
VERSION_LOW = 3  # CE5: (uint16)((FX_CACHE_VER - (int)FX_CACHE_VER) * 10.1f); KCD2 FX_CACHE_VER = 4.3
VERSION_HIGH = 4  # CE5: (uint16)FX_CACHE_VER
HEADER = struct.Struct("<4sIHHIIII")  # SShaderBinHeader, 28 bytes
LOCAL_INFO_HEADER = struct.Struct("<QIiiii4x")  # SShaderBinParamsHeader, 28 bytes + 4 alignment pad = 32

# fmt: off
# Retail keyword table: KEYWORDS[id] = text. None = id never registered (eT_unknown / unused enum slots).
# Duplicate texts are intentional (CE5 registers e.g. "#if" for both eT_if and eT_if_2); lookup takes the
# FIRST match, like CParserBin::fxToken. "ceil" is NOT a keyword: CE5 and KCD2 both register "floor" twice.
KEYWORDS = [
    None, '#include', '#define', '#define', '#undefine', '#fetchinst', '#if', '#ifdef',  # 0
    '#ifndef', '#if', '#ifdef', '#ifndef', '#elif', '#elif', '#endif', '#else',  # 8
    '|', '&', '#warning', '#register_env', '#ifcvar', '#ifncvar', '#elifcvar', '#skip',  # 16
    '#skip_(', '#skip_)', '(', ')', '[', ']', '{', '}',  # 24
    '<', '>', ',', '.', ':', ';', '!', '"',  # 32
    "'", '?', '=', '+', '-', '/', '*', 'dot',  # 40
    'mul', 'sqrt', 'exp', 'log', 'log2', 'sin', 'cos', 'sincos',  # 48
    'floor', 'floor', 'frac', 'lerp', 'abs', 'clamp', 'min', 'max',  # 56
    'length', 'tex2D', 'tex2Dproj', 'tex3D', 'texCUBE', 'SamplerState', 'SamplerComparisonState', 'sampler_state',  # 64
    'Texture2D', 'RWTexture2D', 'RWTexture2DArray', 'Texture2DArray', 'Texture2DMS', 'TextureCube', 'TextureCubeArray', 'Texture3D',  # 72
    'RWTexture3D', 'snorm', 'unorm', 'float', 'float2', 'float3', 'float4', 'float2x4',  # 80
    'float3x4', 'float4x4', 'float3x3', 'half', 'half2', 'half3', 'half4', 'half2x4',  # 88
    'half3x4', 'half4x4', 'half3x3', 'bool', 'int', 'int2', 'int3', 'int4',  # 96
    'uint', 'uint2', 'uint3', 'uint4', 'min16float', 'min16float2', 'min16float3', 'min16float4',  # 104
    'min16float4x4', 'min16float3x4', 'min16float2x4', 'min16float3x3', 'min10float', 'min10float2', 'min10float3', 'min10float4',  # 112
    'min10float4x4', 'min10float3x4', 'min10float2x4', 'min10float3x3', 'min16int', 'min16int2', 'min16int3', 'min16int4',  # 120
    'min12int', 'min12int2', 'min12int3', 'min12int4', 'min16uint', 'min16uint2', 'min16uint3', 'min16uint4',  # 128
    'sampler1D', 'sampler2D', 'sampler3D', 'samplerCUBE', 'const', 'inout', 'struct', 'sampler',  # 136
    'TEXCOORDN', 'TEXCOORD0', 'TEXCOORD1', 'TEXCOORD2', 'TEXCOORD3', 'TEXCOORD4', 'TEXCOORD5', 'TEXCOORD6',  # 144
    'TEXCOORD7', 'TEXCOORD8', 'TEXCOORD9', 'TEXCOORD10', 'TEXCOORD11', 'TEXCOORD12', 'TEXCOORD13', 'TEXCOORD14',  # 152
    'TEXCOORD15', 'TEXCOORD16', 'TEXCOORD17', 'TEXCOORD18', 'TEXCOORD19', 'TEXCOORD20', 'TEXCOORD21', 'TEXCOORD22',  # 160
    'TEXCOORD23', 'TEXCOORD24', 'TEXCOORD25', 'TEXCOORD26', 'TEXCOORD27', 'TEXCOORD28', 'TEXCOORD29', 'TEXCOORD30',  # 168
    'TEXCOORD31', 'TEXCOORDN_centroid', 'TEXCOORD0_centroid', 'TEXCOORD1_centroid', 'TEXCOORD2_centroid', 'TEXCOORD3_centroid', 'TEXCOORD4_centroid', 'TEXCOORD5_centroid',  # 176
    'TEXCOORD6_centroid', 'TEXCOORD7_centroid', 'TEXCOORD8_centroid', 'TEXCOORD9_centroid', 'TEXCOORD10_centroid', 'TEXCOORD11_centroid', 'TEXCOORD12_centroid', 'TEXCOORD13_centroid',  # 184
    'TEXCOORD14_centroid', 'TEXCOORD15_centroid', 'TEXCOORD16_centroid', 'TEXCOORD17_centroid', 'TEXCOORD18_centroid', 'TEXCOORD19_centroid', 'TEXCOORD20_centroid', 'TEXCOORD21_centroid',  # 192
    'TEXCOORD22_centroid', 'TEXCOORD23_centroid', 'TEXCOORD24_centroid', 'TEXCOORD25_centroid', 'TEXCOORD26_centroid', 'TEXCOORD27_centroid', 'TEXCOORD28_centroid', 'TEXCOORD29_centroid',  # 200
    'TEXCOORD30_centroid', 'TEXCOORD31_centroid', 'COLOR0', 'static', None, 'groupshared', 'packoffset', 'register',  # 208
    'return', 'vsregister', 'psregister', 'gsregister', 'dsregister', 'hsregister', 'csregister', 'StructuredBuffer',  # 216
    'RWStructuredBuffer', 'ByteAddressBuffer', 'RWByteAddressBuffer', 'Buffer', 'RWBuffer', 'color', 'Position', 'Allways',  # 224
    'STANDARDSGLOBAL', 'technique', 'string', 'UIName', 'UIDescription', 'UIWidget', 'UIWidget0', 'UIWidget1',  # 232
    'UIWidget2', 'UIWidget3', 'Texture', 'Filter', 'MinFilter', 'MagFilter', 'MipFilter', 'AddressU',  # 240
    'AddressV', 'AddressW', 'BorderColor', 'sRGBLookup', 'LINEAR', 'POINT', 'NONE', 'ANISOTROPIC',  # 248
    'MIN_MAG_MIP_POINT', 'MIN_MAG_MIP_LINEAR', 'MIN_MAG_LINEAR_MIP_POINT', 'COMPARISON_MIN_MAG_LINEAR_MIP_POINT', 'MINIMUM_MIN_MAG_MIP_LINEAR', 'MAXIMUM_MIN_MAG_MIP_LINEAR', 'Clamp', 'Border',  # 256
    'Wrap', 'Mirror', 'Script', '//', 'asm', 'RenderOrder', 'ProcessOrder', 'RenderCamera',  # 264
    'RenderType', 'RenderFilter', 'RenderColorTarget1', 'RenderDepthStencilTarget', 'ClearSetColor', 'ClearSetDepth', 'ClearTarget', 'RenderTarget_IDPool',  # 272
    'RenderTarget_UpdateType', 'RenderTarget_Width', 'RenderTarget_Height', 'GenerateMips', 'PreProcess', 'PostProcess', 'PreDraw', 'WaterReflection',  # 280
    'Panorama', 'WaterPlaneReflected', 'PlaneReflected', 'Current', 'CurObject', 'CurScene', 'RecursiveScene', 'CopyScene',  # 288
    'Refractive', 'ForceRefractionUpdate', 'Heat', 'DepthBuffer', 'DepthBufferTemp', 'DepthBufferOrig', '$ScreenSize', 'WaterReflect',  # 296
    'FogColor', 'Color', 'Depth', '$RT_2D', '$RT_CM', '$RT_Cube', 'pass', 'CustomRE',  # 304
    'Style', 'VertexShader', 'PixelShader', 'GeometryShader', 'HullShader', 'DomainShader', 'ComputeShader', 'ZEnable',  # 312
    'ZWriteEnable', 'CullMode', 'SrcBlend', 'DestBlend', 'AlphaBlendEnable', 'AlphaFunc', 'AlphaRef', 'ZFunc',  # 320
    'ColorWriteEnable', 'IgnoreMaterialState', 'None', 'Disable', 'CCW', 'CW', 'Back', 'Front',  # 328
    'Never', 'Less', 'Equal', 'LEqual', 'LessEqual', 'NotEqual', 'GEqual', 'GreaterEqual',  # 336
    'Greater', 'Always', 'RED', 'GREEN', 'BLUE', 'ALPHA', 'ONE', 'ZERO',  # 344
    'SRC_COLOR', 'SrcColor', 'ONE_MINUS_SRC_COLOR', 'InvSrcColor', 'SRC_ALPHA', 'SrcAlpha', 'ONE_MINUS_SRC_ALPHA', 'InvSrcAlpha',  # 352
    'DST_ALPHA', 'DestAlpha', 'ONE_MINUS_DST_ALPHA', 'InvDestAlpha', 'DST_COLOR', 'DestColor', 'ONE_MINUS_DST_COLOR', 'InvDestColor',  # 360
    'SRC_ALPHA_SATURATE', 'NULL', 'cbuffer', 'PER_BATCH', 'PER_INSTANCE', 'PER_MATERIAL', 'SKIN_DATA', 'INSTANCE_DATA',  # 368
    'ShaderType', 'ShaderDrawType', 'PreprType', 'Public', 'NoPreview', 'LocalConstants', 'Cull', 'SupportsAttrInstancing',  # 376
    'SupportsConstInstancing', 'SupportsDeferredShading', 'SupportsFullDeferredShading', 'Decal', 'DecalNoDepthOffset', 'NoChunkMerging', 'ForceTransPass', 'AfterHDRPostProcess',  # 384
    'AfterPostProcess', 'ForceZpass', 'ForceWaterPass', 'ForceDrawLast', 'ForceDrawFirst', 'ForceDrawAfterWater', 'DepthFixup', 'DepthFixupReplace',  # 392
    'SingleLightPass', 'HWTessellation', 'WaterParticle', 'AlphaBlendShadows', 'ZPrePass', 'WrinkleBlending', 'SupportsHidingGroupsWH', 'SupportsBloodWH',  # 400
    'SupportsGrimeWH', 'SupportsScratchWH', 'Light', 'Shadow', 'Fur', 'General', 'Terrain', 'Overlay',  # 408
    'NoDraw', 'Custom', 'Sky', 'OceanShore', 'Hair', 'Compute', 'ForceGeneralPass', 'EyeOverlay',  # 416
    'Metal', 'Ice', 'Water', 'FX', 'HDR', 'HUD3D', 'Glass', 'Vegetation',  # 424
    'Particle', 'GenerateClouds', 'ScanWater', 'NoLights', 'NoMaterialState', 'PositionInvariant', 'TechniqueZ', 'TechniqueShadowGen',  # 432
    'TechniqueMotionBlur', 'TechniqueCustomRender', 'TechniqueEffectLayer', 'TechniqueDebug', 'TechniqueWaterRefl', 'TechniqueWaterCaustic', 'TechniqueZPrepass', 'TechniqueThickness',  # 440
    'ChainAfterWH', None, 'KeyFrameParams', 'KeyFrameRandColor', 'KeyFrameRandIntensity', 'KeyFrameRandSpecMult', 'KeyFrameRandPosOffset', 'Speed',  # 448
    'Beam', 'LensOptics', 'Cloud', 'Ocean', 'Model', 'StartRadius', 'EndRadius', 'StartColor',  # 456
    'EndColor', 'LightStyle', 'Length', 'RGBStyle', 'Scale', 'Blind', 'SizeBlindScale', 'SizeBlindBias',  # 464
    'IntensBlindScale', 'IntensBlindBias', 'MinLight', 'DistFactor', 'DistIntensityFactor', 'FadeTime', 'Layer', 'Importance',  # 472
    'VisAreaScale', 'Poly', 'Identity', 'FromObj', 'FromLight', 'Fixed', 'ParticlesFile', 'Gravity',  # 480
    'WindDirection', 'WindSpeed', 'WaveHeight', 'DirectionalDependence', 'ChoppyWaveFactor', 'SuppressSmallWavesFactor', '%_TT_TEXCOORD_MATRIX', '%_TT_TEXCOORD_PROJ',  # 488
    '%_TT_TEXCOORD_GEN_OBJECT_LINEAR', '%_TT_TEXCOORD_GEN_WORLD', '%_VT_TYPE', '%_VT_TYPE_MODIF', '%_VT_BEND', '%_VT_DET_BEND', '%_VT_GRASS', '%_VT_WIND',  # 496
    '%_VT_DEPTH_OFFSET', '%_FT_TEXTURE', '%_FT_TEXTURE1', '%_FT_NORMAL', '%_FT_PSIZE', '%_FT_DIFFUSE', '%_FT_SPECULAR', '%_FT_TANGENT_STREAM',  # 504
    '%_FT_QTANGENT_STREAM', '%_FT_SKIN_STREAM', '%_FT_VERTEX_VELOCITY_STREAM', '%_FT0_COP', '%_FT0_AOP', '%_FT0_CARG1', '%_FT0_CARG2', '%_FT0_AARG1',  # 512
    '%_FT0_AARG2', '%_VS', '%_PS', '%_GS', '%_HS', '%_DS', '%_CS', 'x',  # 520
    'y', 'z', 'w', 'r', 'g', 'b', 'a', 'true',  # 528
    'false', '0', '1', '2', '3', '4', '5', '6',  # 536
    '7', '8', '9', '10', '11', '12', '13', '14',  # 544
    '15', 'AnisotropyLevel', 'ORBIS', 'HERCULES', 'DURANGO', 'SCARLETT', 'PCDX11', 'PCDX12',  # 552
    None, 'VULKAN', 'VT_DetailBendingGrass', 'VT_DetailBending', 'VT_WindBending', 'VertexColors', 's0', 's1',  # 560
    's2', 's3', 's4', 's5', 's6', 's7', 's8', 's9',  # 568
    's10', 's11', 's12', 's13', 's14', 's15', 't0', 't1',  # 576
    't2', 't3', 't4', 't5', 't6', 't7', 't8', 't9',  # 584
    't10', 't11', 't12', 't13', 't14', 't15', 'Global', 'Load',  # 592
    'Sample', 'Gather', 'GatherRed', 'GatherGreen', 'GatherBlue', 'GatherAlpha', 'Billboard', 'DebugHelper',  # 600
    'globallycoherent', 'CRY_EARLY_DEPTH', 'CRY_RE_Z', 'CRY_EARLY_DEPTH_OR_RE_Z', 'CRY_WAVE_32',  # 608
]
# fmt: on
KEYWORD_COUNT = len(KEYWORDS)  # eT_max = 613; eT_user_first = 614

# first registered id wins (CParserBin::fxToken scans ids upward with strcmp)
KEYWORD_ID: dict[bytes, int] = {}
for _i, _k in enumerate(KEYWORDS):
    if _k is not None:
        KEYWORD_ID.setdefault(_k.encode("latin-1"), _i)

# keyword ids with special handling in the tokenizer (same values as CE5's EToken)
T_UNKNOWN = 0
T_INCLUDE = 1
T_DEFINE = 2
T_DEFINE_2 = 3  # "#define %_NAME ..." (a shader-gen mask macro)
T_IF, T_IFDEF, T_IFNDEF = 6, 7, 8
T_IF_2, T_IFDEF_2, T_IFNDEF_2 = 9, 10, 11  # second-pass variants (condition mentions a %_ runtime flag)
T_ELIF, T_ELIF_2 = 12, 13
T_SKIP = 23
T_SEMICOLON, T_BR_CV_1, T_BR_CV_2 = 37, 30, 31

_SECOND_PASS = {T_IF: T_IF_2, T_IFDEF: T_IFDEF_2, T_IFNDEF: T_IFNDEF_2, T_ELIF: T_ELIF_2}


def crc32(data: bytes) -> int:
    """CCrc32::Compute and CCryPak::ComputeCRC are both the standard zlib CRC-32."""
    return zlib.crc32(data) & 0xFFFFFFFF


# ---------------------------------------------------------------------------------------------------------------
# Character classes. CE5 walks a plain `char*`, and `char` is SIGNED on MSVC: bytes >= 0x80 are negative. That
# matters in three places, reproduced here: SkipCharacters() treats them like control characters (skipped),
# SkipChar() treats them as ordinary identifier characters, and "<= 0x20" loops (shFill, macro trimming)
# treat them as whitespace.
# ---------------------------------------------------------------------------------------------------------------


def _build_skipchar_table() -> list[bool]:
    table = []
    for c in range(256):
        if c >= 0x80:
            table.append(False)  # (unsigned)(char)c is huge: no range matches
            continue
        table.append(
            c <= 0x20
            or 0x21 <= c <= 0x22  # ! "
            or 0x26 <= c <= 0x2F  # & ' ( ) * + , - . /
            or 0x3A <= c <= 0x3F  # : ; < = > ?
            or c in (0x5B, 0x5D)  # [ ]
            or 0x7B <= c <= 0x7D  # { | }
        )
    return table


SKIP_CHAR = _build_skipchar_table()  # CE5 SkipChar(): token delimiters
WS_MAIN = b" "  # SaveBinShader's local kWhiteSpace
WS_GLOBAL = b" ,"  # Parser.cpp's global kWhiteSpace (used by SkipComments, shFill, fxFillCR)


def _signed_le_space(c: int) -> bool:
    """`(char)c <= 0x20` with signed char."""
    return c <= 0x20 or c >= 0x80


def _skip_characters(b: bytes, p: int, to_skip: bytes) -> int:
    """SkipCharacters(): skip bytes that are < 0x20 (signed, so also >= 0x80) or listed in to_skip."""
    while True:
        c = b[p]
        if c == 0:
            return p
        if 0x20 <= c < 0x80 and c not in to_skip:
            return p
        p += 1


def _strchr(b: bytes, p: int, ch: int) -> int | None:
    q = b.find(bytes([ch]), p, len(b) - 1)  # the buffer holds exactly one NUL, at the end
    return None if q < 0 else q


def _skip_comments(b: bytes, p: int | None) -> int | None:
    """SkipComments(buf, true). Block comments nest (CE5 counts '/*' and '*/' pairs)."""
    while p is not None:
        if b[p] == 0x2F and b[p + 1] == 0x2F:  # '//'
            p = _strchr(b, p, 0x0A)
            if p is not None:
                p = _skip_characters(b, p, WS_GLOBAL)
        elif b[p] == 0x2F and b[p + 1] == 0x2A:  # '/*'
            m = 0
            while True:
                p = _strchr(b, p, 0x2A)
                if p is None:
                    break
                if p > 0 and b[p - 1] == 0x2F:
                    p += 1
                    m += 1
                elif b[p + 1] == 0x2F:
                    p += 2
                    m -= 1
                else:
                    p += 1
                if m == 0:
                    break
            if p is None:  # "Comment lines aren't closed": CE5 leaves the NULL pointer, parsing stops
                return None
            p = _skip_characters(b, p, WS_GLOBAL)
        else:
            break
    return p


def _next_token(b: bytes, p: int) -> tuple[bytes, int]:
    """CParserBin::NextToken(): a run of non-delimiters, else one delimiter character."""
    start = p
    while True:
        c = b[p]
        if c == 0 or SKIP_CHAR[c]:
            break
        p += 1
        if c == 0x2F:  # unreachable in practice ('/' is a delimiter) but kept from CE5
            break
    if p == start:
        c = b[p]
        if c != 0x20:
            p += 1
            return (bytes([c]) if c else b""), p
        return b"", p
    return b[start:p], p


def _is_first_pass(b: bytes, p: int) -> bool:
    """fxIsFirstPass(): False when the rest of the directive line has a word starting with '%_'."""
    p = _skip_characters(b, p, WS_GLOBAL)  # fxFillCR
    end = p
    while b[end] != 0 and b[end] != 0x0A:
        end += 1
    line = b[p:end]
    i, n = 0, len(line)
    while i < n:  # fxFillPr loop
        while i < n and SKIP_CHAR[line[i]]:
            i += 1
        s = i
        while i < n and not SKIP_CHAR[line[i]]:
            i += 1
        if line[s : s + 2] == b"%_":
            return False
    return True


class Tokenizer:
    """Port of CShaderManBin::SaveBinShader's token loop. Produces the token list and the string table."""

    def __init__(self):
        self.tokens: list[int] = []
        self.table: dict[int, bytes] = {}

    def _user(self, tok: int, text: bytes) -> int:
        """CParserBin::NewUserToken(): keywords pass through, anything else becomes CRC-32(text)."""
        if tok != T_UNKNOWN:
            return tok
        tok = crc32(text)
        self.table.setdefault(tok, text)  # First spelling wins when two names share a CRC.
        return tok

    def _word(self, text: bytes) -> int:
        return self._user(KEYWORD_ID.get(text, T_UNKNOWN), text)

    def run(self, source: bytes) -> list[int]:
        b = source.replace(b"\r", b" ") + b"\0"  # RemoveCR(); the file is read raw and zero-terminated
        b = b[: b.index(0) + 1]  # an embedded NUL ends the text, as in C
        toks = self.tokens
        p: int | None = 0
        while p is not None and b[p]:
            p = _skip_characters(b, p, WS_MAIN)
            p = _skip_comments(b, p)
            if p is None or not b[p]:
                break
            text, p = _next_token(b, p)
            tok = self._word(text)
            toks.append(tok)
            p = _skip_characters(b, p, WS_MAIN)
            p = _skip_comments(b, p)
            if p is None:
                break

            if tok == T_INCLUDE:
                # #include "Name.cfi" -> [T_INCLUDE, CRC("Name")]: quotes dropped, extension removed
                p = _skip_characters(b, p, WS_MAIN)
                brak = b[p]
                p += 1
                s = p
                while b[p] != brak:  # CE5 bug kept: '<' is closed by another '<', not '>'
                    if _signed_le_space(b[p]):
                        break
                    p += 1
                name = b[s:p]
                if b[p] == brak:
                    p += 1
                cut = name.rfind(b".")  # PathUtil::RemoveExtension: last '.' not followed by / \ or :
                if cut >= 0 and not any(ch in b"/\\:" for ch in name[cut:]):
                    name = name[:cut]
                toks.append(self._word(name))
            elif tok in _SECOND_PASS:
                # #if/#ifdef/#ifndef/#elif whose line mentions a %_ flag -> the *_2 (second pass) variant
                if not _is_first_pass(b, p):
                    toks[-1] = _SECOND_PASS[tok]
            elif tok == T_DEFINE:
                p = self._define(b, p)
        if not toks or not toks[0]:
            toks.append(T_SKIP)
        return toks

    def _define(self, b: bytes, p: int) -> int:
        """#define NAME body -> [T_DEFINE or T_DEFINE_2, CRC(NAME), body tokens..., 0]."""
        toks = self.tokens
        p = _skip_characters(b, p, WS_GLOBAL)  # shFill: name = run of bytes > 0x20 (signed)
        s = p
        while not _signed_le_space(b[p]) and b[p]:
            p += 1
        name = b[s:p]
        if name[:1] == b"%":
            toks[-1] = T_DEFINE_2
        toks.append(self._user(T_UNKNOWN, name))  # macro name is always hashed, even if it is a keyword

        while b[p] in (0x20, 0x09):
            p += 1
        macro = bytearray()
        while b[p] != 0x0A and b[p] != 0:  # (CE5 would run past a final line without '\n')
            if b[p] == 0x5C:  # '\\' line continuation -> '\n', rest of the physical line dropped
                macro.append(0x0A)
                while b[p] != 0x0A and b[p] != 0:
                    p += 1
                if b[p]:
                    p += 1
                continue
            macro.append(b[p])
            p += 1
        while macro and _signed_le_space(macro[-1]):
            macro.pop()
        m = bytes(macro) + b"\0"
        q: int | None = 0
        while q is not None and m[q]:
            q = _skip_characters(m, q, WS_MAIN)
            q = _skip_comments(m, q)
            if q is None or not m[q]:
                break
            text, q = _next_token(m, q)
            tok = self._word(text)
            if tok in _SECOND_PASS and not _is_first_pass(m, q):
                tok = _SECOND_PASS[tok]
            toks.append(tok)
        toks.append(0)  # macro terminator
        return p


def tokenize(source: bytes) -> tuple[list[int], dict[int, bytes]]:
    t = Tokenizer()
    t.run(source)
    return t.tokens, t.table


# ---------------------------------------------------------------------------------------------------------------
# CRC (SShaderBin::ComputeCRC, retail sub_180918A8C)
# ---------------------------------------------------------------------------------------------------------------


def _tokens_bytes(tokens: list[int]) -> bytes:
    return struct.pack("<%dI" % len(tokens), *tokens)


class IncludeResolver:
    """Finds an included file by name for the header CRC, like GetBinShader(name, bInclude=true) does in-game.

    dirs are searched in order; in each, <name>.cfib (a game binary, parsed) wins over <name>.cfi (a source,
    tokenized here). Names are matched case-insensitively. Both give the same CRC for unmodified retail files."""

    def __init__(self, dirs=()):
        self.files: dict[str, str] = {}
        for d in dirs:
            if not d or not os.path.isdir(d):
                continue
            listing = {f.lower(): os.path.join(d, f) for f in os.listdir(d)}
            for low, path in sorted(listing.items(), key=lambda kv: not kv[0].endswith(".cfib")):
                if low.endswith((".cfib", ".cfi")):
                    self.files.setdefault(low.rsplit(".", 1)[0], path)
        self.cache: dict[str, tuple[list[int], dict[int, bytes]] | None] = {}
        self.crc_cache: dict[str, int] = {}
        self.missing: set[str] = set()

    def load(self, name: str):
        key = name.lower()
        if key not in self.cache:
            path = self.files.get(key)
            if path is None:
                self.cache[key] = None
                self.missing.add(name)
            elif path.lower().endswith(".cfib"):
                sb = parse(path)
                self.cache[key] = (sb["tokens"], sb["strings"])
            else:
                with open(path, "rb") as fh:
                    self.cache[key] = tokenize(fh.read())
        return self.cache[key]

    def crc_of(self, name: str, _stack=()) -> int | None:
        key = name.lower()
        if key in self.crc_cache:
            return self.crc_cache[key]
        loaded = self.load(name)
        if loaded is None or key in _stack:
            return None
        value = compute_crc(loaded[0], loaded[1], self, _stack + (key,))
        self.crc_cache[key] = value
        return value


def compute_crc(tokens: list[int], table: dict[int, bytes], resolver: IncludeResolver | None, _stack=()) -> int:
    """Header CRC = CRC-32 over the little-endian token array, PLUS (u32 wrap-around addition) the same value
    computed recursively for every '#include' token occurrence (each occurrence counts, duplicates too).
    An include that cannot be found is skipped, exactly like the engine."""
    if not tokens:
        return 0
    value = crc32(_tokens_bytes(tokens))
    i = 0
    last = len(tokens) - 1
    while True:
        try:
            i = tokens.index(T_INCLUDE, i, last + 1)  # CParserBin::FindToken(nCur, size - 1, eT_include)
        except ValueError:
            break
        i += 1
        if resolver is None:
            continue
        name_tok = tokens[i] if i <= last else 0
        name = KEYWORDS[name_tok] if name_tok < KEYWORD_COUNT else table.get(name_tok, b"").decode("latin-1")
        sub = resolver.crc_of(name or "", _stack)
        if sub is not None:
            value = (value + sub) & 0xFFFFFFFF
    return value


# ---------------------------------------------------------------------------------------------------------------
# Serialization
# ---------------------------------------------------------------------------------------------------------------


def serialize(tokens, table, crc, source_crc, local_info: bytes = b"") -> bytes:
    body = _tokens_bytes(tokens)
    strings = b"".join(struct.pack("<I", t) + table[t] + b"\0" for t in sorted(table))
    off_strings = HEADER.size + len(body)
    off_local = off_strings + len(strings)  # FTell after the string table, written before any local info
    header = HEADER.pack(MAGIC, crc, VERSION_LOW, VERSION_HIGH, off_strings, off_local, len(tokens), source_crc)
    return header + body + strings + local_info


def build(source_text, name: str = "", is_include: bool = False, include_dirs=(), *,
          resolver: IncludeResolver | None = None, local_info: bytes = b"", strict: bool = True) -> bytes:
    """Tokenize CryFX source into .cfxb (is_include=False) or .cfib (is_include=True) bytes.

    source_text: bytes (preferred: exact) or str (encoded as UTF-8). The source CRC is taken over these bytes.
    name: shader name without extension; not stored in the file (the file name carries it), used only as the
          include-cycle guard for a .cfi.
    include_dirs / resolver: where the #include'd files are found (.cfib preferred over .cfi per directory);
          they feed the header CRC, which the game recomputes on load and rejects the file if it differs.
    strict: raise if an #include cannot be resolved (the CRC would then be wrong for the game).
    local_info: engine param cache bytes to append verbatim (.cfxb only). Only valid for an UNCHANGED token
          stream: its function entries are indices into this file's code fragments."""
    data = source_text.encode("utf-8") if isinstance(source_text, str) else bytes(source_text)
    if resolver is None:
        resolver = IncludeResolver(include_dirs)
    tokens, table = tokenize(data)
    missing_before = set(resolver.missing)
    # cycle guard holds include names only: Foo.cfx including Foo.cfi is normal (18 retail shaders do it)
    crc = compute_crc(tokens, table, resolver, (name.lower(),) if (name and is_include) else ())
    new_missing = resolver.missing - missing_before
    if strict and new_missing:
        raise FileNotFoundError("unresolved #include(s) %s: pass include_dirs holding the game's .cfib (or the "
                                ".cfi sources)" % ", ".join(sorted(new_missing)))
    if is_include:
        local_info = b""  # the engine never reads or appends local info for includes
    return serialize(tokens, table, crc, crc32(data), local_info)


def parse(path_or_bytes) -> dict:
    """Decode a .cfxb/.cfib into a dict: header fields, tokens, strings {token: bytes}, local_info entries."""
    if isinstance(path_or_bytes, (bytes, bytearray)):
        d = bytes(path_or_bytes)
    else:
        with open(path_or_bytes, "rb") as fh:
            d = fh.read()
    magic, crc, vlo, vhi, off_str, off_local, ntok, src_crc = HEADER.unpack_from(d, 0)
    if magic != MAGIC:
        raise ValueError("bad magic %r" % magic)
    tokens = list(struct.unpack_from("<%dI" % ntok, d, HEADER.size))
    strings: dict[int, bytes] = {}
    p = off_str
    while p < off_local:
        (t,) = struct.unpack_from("<I", d, p)
        e = d.index(b"\0", p + 4)
        strings[t] = d[p + 4 : e]
        p = e + 1
    local = []
    p = off_local
    while p + LOCAL_INFO_HEADER.size <= len(d):
        mask, nm, npar, nsam, ntex, nfun = LOCAL_INFO_HEADER.unpack_from(d, p)
        p += LOCAL_INFO_HEADER.size
        arrs = []
        for n in (npar, nsam, ntex, nfun):
            arrs.append(list(struct.unpack_from("<%di" % n, d, p)))
            p += 4 * n
        local.append({"gen_mask": mask, "name_crc": nm, "params": arrs[0], "samplers": arrs[1],
                      "textures": arrs[2], "funcs": arrs[3]})
    return {
        "header": {"magic": magic, "crc": crc, "version_low": vlo, "version_high": vhi,
                   "offset_string_table": off_str, "offset_local_info": off_local, "token_count": ntok,
                   "source_crc": src_crc},
        "tokens": tokens,
        "strings": strings,
        "local_info": local,
        "local_info_bytes": d[off_local:],
        "size": len(d),
    }


_VARIANT_TEXT = {T_DEFINE_2: "#define_2", T_IF_2: "#if_2", T_IFDEF_2: "#ifdef_2", T_IFNDEF_2: "#ifndef_2",
                 T_ELIF_2: "#elif_2"}  # dump-only spellings for the ids that share text with another keyword


def token_text(tok: int, strings: dict[int, bytes]) -> str:
    if tok < KEYWORD_COUNT:
        return _VARIANT_TEXT.get(tok) or KEYWORDS[tok] or "<%d>" % tok
    s = strings.get(tok)
    return s.decode("latin-1") if s is not None else "<0x%08X>" % tok


def detokenize(tokens, strings) -> str:
    """Readable text in the spirit of CParserBin::ConvertToAscii (macro terminators become newlines)."""
    out, line = [], []
    for t in tokens:
        if t == 0:
            out.append(" ".join(line))
            line = []
            continue
        line.append(token_text(t, strings))
        if t in (T_SEMICOLON, T_BR_CV_1, T_BR_CV_2):
            out.append(" ".join(line))
            line = []
    if line:
        out.append(" ".join(line))
    return "\n".join(out)


# ---------------------------------------------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------------------------------------------


def _explain(ref: bytes, out: bytes) -> str:
    """Name the region of the first differing byte, and the tokens involved."""
    n = min(len(ref), len(out))
    off = next((i for i in range(n) if ref[i] != out[i]), n)
    try:
        r = parse(ref)
    except Exception:  # noqa: BLE001
        return "offset 0x%X (reference unparsable)" % off
    h = r["header"]
    if off < HEADER.size:
        field = ["magic"] * 4 + ["crc"] * 4 + ["version_low"] * 2 + ["version_high"] * 2 + \
                ["offset_string_table"] * 4 + ["offset_local_info"] * 4 + ["token_count"] * 4 + ["source_crc"] * 4
        return "offset 0x%X in header field %s" % (off, field[off])
    if off < h["offset_string_table"]:
        i = (off - HEADER.size) // 4
        o = parse(out)
        a = token_text(r["tokens"][i], r["strings"]) if i < len(r["tokens"]) else "<end>"
        b = token_text(o["tokens"][i], o["strings"]) if i < len(o["tokens"]) else "<end>"
        ctx = " ".join(token_text(t, r["strings"]) for t in r["tokens"][max(0, i - 8) : i])
        return "offset 0x%X token #%d: expected %r got %r (after: ...%s)" % (off, i, a, b, ctx)
    if off < h["offset_local_info"]:
        return "offset 0x%X in string table" % off
    return "offset 0x%X in local info (engine param cache)" % off


def cmd_verify(args) -> int:
    srcs = {f.lower(): os.path.join(args.srcdir, f) for f in os.listdir(args.srcdir)
            if f.lower().endswith((".cfx", ".cfi"))}
    bins = sorted(f for f in os.listdir(args.bindir) if f.lower().endswith((".cfxb", ".cfib")))
    # includes come from the SOURCE dir only, so the header CRC is checked without help from the binaries
    resolver = IncludeResolver([args.srcdir])
    exact = with_cache = bad = 0
    for fn in bins:
        src = srcs.get(fn.lower()[:-1])  # foo.cfxb -> foo.cfx, foo.cfib -> foo.cfi
        if src is None:
            print("%-46s MISMATCH no source file" % fn)
            bad += 1
            continue
        with open(os.path.join(args.bindir, fn), "rb") as fh:
            ref = fh.read()
        with open(src, "rb") as fh:
            data = fh.read()
        is_inc = fn.lower().endswith(".cfib")
        name = os.path.splitext(os.path.basename(src))[0]
        out = build(data, name, is_inc, resolver=resolver, strict=False)
        if out == ref:
            exact += 1
            print("%-46s OK" % fn)
            continue
        # The game's cache build appends a param cache after the string table (SaveBinShaderLocalInfo). It is
        # not produced from the source, so check that the rest is exact and that re-appending it restores the file.
        off_local = HEADER.unpack_from(out, 0)[5]
        if len(ref) > off_local and ref[:off_local] == out:
            tail = parse(ref)
            if build(data, name, is_inc, resolver=resolver, strict=False,
                     local_info=tail["local_info_bytes"]) == ref:
                with_cache += 1
                print("%-46s OK   (+ engine param cache: %d bytes, %d entries, carried over)"
                      % (fn, len(tail["local_info_bytes"]), len(tail["local_info"])))
                continue
        bad += 1
        print("%-46s MISMATCH %s" % (fn, _explain(ref, out)))
    print("\n%d/%d byte-exact: %d built from source alone, %d after re-appending their engine-written param "
          "cache; %d mismatched" % (exact + with_cache, len(bins), exact, with_cache, bad))
    if resolver.missing:
        print("unresolved includes: %s" % ", ".join(sorted(resolver.missing)))
    return 0 if bad == 0 else 1


def cmd_build(args) -> int:
    src = args.input
    is_inc = src.lower().endswith(".cfi")
    name = os.path.splitext(os.path.basename(src))[0]
    with open(src, "rb") as fh:
        data = fh.read()
    dirs = list(args.include or []) or [os.path.dirname(os.path.abspath(src))]
    local = parse(args.local_info_from)["local_info_bytes"] if args.local_info_from else b""
    try:
        out = build(data, name, is_inc, dirs, local_info=local, strict=not args.allow_missing)
    except FileNotFoundError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    with open(args.output, "wb") as fh:
        fh.write(out)
    r = parse(out)
    print("%s: %d bytes, %d tokens, %d strings, crc 0x%08X, source crc 0x%08X"
          % (args.output, len(out), len(r["tokens"]), len(r["strings"]), r["header"]["crc"],
             r["header"]["source_crc"]))
    return 0


def cmd_header(args) -> int:
    """Tokenize a .cfx and write it as a C++ header holding the bytes (the mod embeds its shader binary)."""
    src = args.input
    name = os.path.splitext(os.path.basename(src))[0]
    with open(src, "rb") as fh:
        data = fh.read()
    dirs = list(args.include or []) or [os.path.dirname(os.path.abspath(src))]
    try:
        out = build(data, name, False, dirs)
    except FileNotFoundError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    r = parse(out)
    guard = re.sub(r"[^A-Z0-9]", "_", os.path.basename(args.output).upper())
    lines = [
        "/**",
        " * @file %s" % os.path.basename(args.output).replace("\\", "/"),
        " * @brief GENERATED by scripts/cfxb.py header from %s; do not edit." % os.path.basename(src),
        " *",
        " * The tokenized binary (.cfxb) of the shader: %d bytes, %d tokens, crc 0x%08X, source crc 0x%08X."
        % (len(out), len(r["tokens"]), r["header"]["crc"], r["header"]["source_crc"]),
        " */",
        "#ifndef HENRYSENSES_%s" % guard,
        "#define HENRYSENSES_%s" % guard,
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "namespace HenrySenses",
        "{",
        "    // The shader's name, and so its file name: the stem with the tokens' crc, so a changed shader loads under a",
        "    // new name (the game keeps a loaded shader by name for the session).",
        "    inline constexpr char %s_NAME[] = \"%s_%08X\";" % (args.symbol, name, r["header"]["crc"]),
        "",
        "    // clang-format off",
        "    inline constexpr std::array<std::uint8_t, %d> %s = {{" % (len(out), args.symbol),
    ]
    for i in range(0, len(out), 16):
        lines.append("        " + ", ".join("0x%02X" % b for b in out[i:i + 16]) + ",")
    lines += ["    }};", "    // clang-format on", "} // namespace HenrySenses", "", "#endif // HENRYSENSES_%s" % guard, ""]
    with open(args.output, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))
    print("%s: %d bytes, crc 0x%08X" % (args.output, len(out), r["header"]["crc"]))
    return 0


def cmd_dump(args) -> int:
    r = parse(args.file)
    for k, v in r["header"].items():
        print("%-20s %s" % (k, ("0x%08X" % v) if isinstance(v, int) and k.endswith("crc") else v))
    print("%-20s %d" % ("strings", len(r["strings"])))
    print("%-20s %d entries, %d bytes" % ("local_info", len(r["local_info"]), len(r["local_info_bytes"])))
    if args.strings:
        for t in sorted(r["strings"]):
            print("  0x%08X %s" % (t, r["strings"][t].decode("latin-1")))
    if args.local:
        for e in r["local_info"]:
            print("  mask 0x%016X name %s params %d samplers %d textures %d funcs %d" % (
                e["gen_mask"], token_text(e["name_crc"], r["strings"]), len(e["params"]), len(e["samplers"]),
                len(e["textures"]), len(e["funcs"])))
    if not args.no_text:
        print(detokenize(r["tokens"], r["strings"]))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("verify", help="rebuild every binary from its source and compare")
    v.add_argument("srcdir")
    v.add_argument("bindir")
    b = sub.add_parser("build", help="tokenize one .cfx/.cfi into .cfxb/.cfib")
    b.add_argument("input")
    b.add_argument("output")
    b.add_argument("-I", "--include", action="append", metavar="DIR",
                   help="dir holding included .cfib (game binaries, preferred) or .cfi sources; repeatable, "
                        "searched in order (default: the input file's dir)")
    b.add_argument("--allow-missing", action="store_true",
                   help="build even if an #include is not found (header CRC will not match in-game)")
    b.add_argument("--local-info-from", metavar="CFXB",
                   help="append the engine param cache of an existing .cfxb (only if the tokens are unchanged)")
    h = sub.add_parser("header", help="tokenize one .cfx into a C++ header holding the .cfxb bytes")
    h.add_argument("input")
    h.add_argument("output")
    h.add_argument("--symbol", required=True, help="name of the std::array constant")
    h.add_argument("-I", "--include", action="append", metavar="DIR", help="as for build")
    d = sub.add_parser("dump", help="print a .cfxb/.cfib")
    d.add_argument("file")
    d.add_argument("--strings", action="store_true")
    d.add_argument("--local", action="store_true")
    d.add_argument("--no-text", action="store_true")
    args = ap.parse_args(argv)
    return {"verify": cmd_verify, "build": cmd_build, "header": cmd_header, "dump": cmd_dump}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
