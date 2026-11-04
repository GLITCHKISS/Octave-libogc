# GX programs (MaterialLite, GameCube)

A MaterialLite can carry a **raw GX program**: a block of GX state that the
GameCube renderer sets as it stands when the material is bound, in place of
everything the material's own settings would make. It is for reproducing
materials from other GameCube games' J3D data (BMD/BDL `MAT3`) exactly: TEV
stages, texgens and texture matrices, colour channels, TEV and konst registers,
indirect texturing, alpha compare, blend and Z mode.

- Only the GameCube uses it. The editor (Vulkan), the PC and the 3DS draw the
  material by its own settings, as before, so give those a sensible look too.
- There is no editing UI. A tool writes the block into the asset; the editor
  keeps it when it saves the material again. Lua: `MaterialLite:HasGxProgram()`.
- Code: `Engine/Source/Engine/Assets/GxProgram.h` (the structs and the texture
  matrix), `MaterialLite.cpp` (the stream), `Graphics/GX/GxUtils.cpp`
  (`BindGxProgram`).

## Not covered yet

Most of GX's per-material state is in the block. What isn't, so far:

- **Special texture-coordinate modes**: projection mapping, environment
  (sphere) maps and bump-shifted coordinates (`GX_TG_BUMP0-7`), and J3D's
  effect matrices (the `effect` 4 x 4 in a texture matrix). Only map mode 0 and
  the screen texgen (matrix byte 3, below) are made.
- **Write masks and destination alpha**: `GX_SetColorUpdate` /
  `GX_SetAlphaUpdate` stay as the renderer sets them, and `GX_SetDstAlpha` is
  not set.
- **J3D's fog block and dither**: fog follows the material's Apply Fog flag.
- **Lights of its own**: a lit channel uses the scene's lights, not a J3D light
  mask.

These are rarely needed for materials copied from J3D data; the rest (TEV,
texgens, colour channels, registers, indirect stages, alpha compare, blend, Z,
cull) is applied as it stands.

## What the rest of the material still does

The program replaces the material's TEV setup, but the engine still reads
these MaterialLite settings:

| Setting | Why |
|---|---|
| Texture slots 0-3 and **Num Textures** | Texture map *n* is slot *n*. A slot at or past Num Textures reads the white texture, and so do maps 4-7. A texture's own filter, wrap, LOD bias and mips are used as they are. |
| **Blend Mode** | Picks the draw pass and the sorting (opaque, then translucent back to front). Set Translucent or Additive for a program that blends, even though the program sets the blend itself. |
| **Shading Model** | Lit or Toon if any channel is lit (`enable` 1). The draw only works out its lights, and loads its normal matrix, for a material that isn't Unlit. |
| **Apply Fog** | The scene's fog, on or off, as for any material. J3D's own fog block is not used. |
| **Cull Mode** | Used when the program's cull byte is 0xFF. |
| Sort Priority | As for any material. |

Everything else (colour, opacity, TEV modes, UV offset and scale, alpha band,
specular, Fresnel, warp) is ignored on the GameCube. Refraction (`SetRefract`,
set at runtime) makes texture map 0 the frame copy: see below.

## How it is bound

- **Texgens**: texgen *i* is `GX_TEXCOORD0 + i`, from its source through
  `GX_TEXMTX0 + i * 3` (or `GX_IDENTITY` when its matrix byte is 0). The matrix
  is J3D's texture SRT (below), its scroll animated by the game's elapsed time
  (it stops while the game is paused).
- Sources: `GX_TG_TEX0` (4) is the mesh's UV 0, `GX_TG_TEX1` (5) its UV 1, as
  they are stored in the mesh (no flip). `GX_TG_POS` (0) and `GX_TG_NRM` (1) are
  the vertex's own, in the mesh's space. A draw whose vertices have no normals
  (particles) reads UV 0 for `NRM` and `TEX1`.
- **TEV stages**: each set exactly as given (order, colour and alpha in/op,
  konst selects, swap modes, indirect), then the swap tables.
- **Indirect texturing**: `GX_SetNumIndStages(numIndStages)`, each indirect
  stage's `GX_SetIndTexOrder` and `GX_SetIndTexCoordScale`, the three
  matrices with `GX_SetIndTexMatrix(GX_ITM_0 + m, m, scaleExp)`, and every
  TEV stage's `GX_SetTevIndirect` (all zeroes is a direct stage). J3D keeps
  its indirect matrices in the same form, so they copy straight across.
- **Registers**: TEV colours with `GX_SetTevColorS10` (REG0-2, then PREV),
  konst colours with `GX_SetTevKColor`, the channels' material and ambient
  colours with `GX_SetChanMatColor` / `GX_SetChanAmbColor`.
- **Channels**: `GX_SetNumChans(numChans)` (at least 1: a draw with no channel
  and no texgen hangs the GPU) and `GX_SetChanCtrl` for COLOR0, ALPHA0, COLOR1
  and ALPHA1. A lit channel takes the draw's lights (the scene's, by its
  lighting channels), not J3D's light mask. A `GX_SRC_VTX` source reads the
  mesh's vertex colour (CLR0) on channel 0; it reads the register on channel 1,
  or when the mesh has no vertex colours. Octave's colour scale (lights over 1)
  is not applied.
- **Alpha compare, blend, Z mode, Z compare location**: as given. Colour
  update on, alpha update off, as for every material.
- **Cull**: `GX_SetCullMode(cull)` as given, or the material's Cull Mode for
  0xFF. Octave's meshes wind the other way round from GX: a mesh imported the
  usual way has its back faces culled by `GX_CULL_FRONT` (1). If the tool keeps
  the J3D mesh's own winding, J3D's cull mode is right as it is.
- Before a program, the engine turns off any indirect texturing the last
  material left on. After it, the next material (or any other TEV setup) turns
  the program's off again (`GxResetIndirect`), and puts back the swap modes
  and swap table 0 that every other TEV setup takes as it finds them.
- **The frame copy**: a texgen with matrix byte 3 is the pixel's own place on
  the screen (the position through `GX_TEXMTX9`, projected), and with
  `MaterialLite:SetRefract(true)` texture map 0 is the frame as it was before
  the translucent draws. That is how a J3D material reading an EFB copy of the
  frame (a warp or a haze, with an indirect stage) can be drawn. Static meshes
  only: they are the draws that load `GX_TEXMTX9` for a refraction material.
  Set the Blend Mode to Translucent, since the copy is taken before those
  draws.

## The texture matrix

For map mode 0 (no mapping), as J3D computes it (noclip's `calcTexMtx_Basic`
and `calcTexMtx_Maya`, `src/Common/JSYSTEM/J3D/J3DLoader.ts`). With
`theta = rotation` in radians, `c = cos theta`, `s = sin theta`:

- **Basic** (matrix byte 1, J3D texture matrix info top bit clear):

  ```
  row S: [ scaleS*c, -scaleS*s, 0, translationS + centerS - (scaleS*c*centerS - scaleS*s*centerT) ]
  row T: [ scaleT*s,  scaleT*c, 0, translationT + centerT - (scaleT*s*centerS + scaleT*c*centerT) ]
  ```

  That is `[s' t'] = diag(scaleS, scaleT) R(theta) ([s t] - center) + center + translation`.

- **Maya** (matrix byte 2, top bit set; the centre is not used):

  ```
  row S: [  scaleS*c, scaleS*s, 0, scaleS*(-0.5c - (0.5s - 0.5) - translationS) ]
  row T: [ -scaleT*s, scaleT*c, 0, scaleT*(-0.5c + (0.5s - 0.5) + translationT) + 1 ]
  ```

A 3x4 texgen's third row is `0 0 1 0`. Column 2 is 0 because GX feeds a texture
coordinate in as `(s, t, 1, 1)`. The scroll adds `scroll * time` to the
translation, wrapped by 2 so the numbers stay small (a whole period for a
repeating texture, and for a mirrored one). Don't scroll a clamped texture.

Rotation is stored in **degrees**: J3D keeps it as an s16 where 0x7FFF is half
a turn, so `degrees = s16 / 0x7FFF * 180`.

Other map modes (sphere maps, projections) need matrices made per object and
are not supported yet, apart from the screen (matrix byte 3, above).

## Binary layout

The block follows `mSharpSpecular` at the end of a MaterialLite asset, from
asset version 22 (`ASSET_VERSION_GX_PROGRAM`, `Engine/Source/Engine/Asset.h`).
An asset written at version 22 or later always has the `present` byte. Older
assets load as before.

The stream is **little-endian** (the GameCube's reader swaps). Types: `u8`
and `s8` 1 byte, `s16` 2 bytes, `f32` 4-byte IEEE float. There is no padding or
alignment anywhere. Every enum value is GX's own number (libogc's `gx.h`, which
uses the same numbers as Nintendo's SDK and J3D's data).

| Bytes | Field | Notes |
|---|---|---|
| 1 | `u8 present` | 0: no program, nothing more follows. 1: the rest follows. |
| 1 | `u8 numTevStages` | 1-16 |
| 1 | `u8 numTexGens` | 0-8 |
| 1 | `u8 numChans` | 0-2 (0 is drawn as 1) |
| 25 x numTevStages | stages | see below |
| 1 | `u8 numIndStages` | 0-4 |
| 4 x numIndStages | indirect stages | `u8 texCoord, texMap, scaleS, scaleT` (`GX_ITS_*` 0-8) |
| 75 | indirect matrices | always 3 (`GX_ITM_0-2`), each `f32 m00, m01, m02, m10, m11, m12` then `s8 scaleExp` |
| 9 x numTevStages | TEV stage indirect | one per stage, in stage order, see below |
| 39 x numTexGens | texgens | see below |
| 16 | swap tables | 4 tables (`GX_TEV_SWAP0-3`) x `u8 r, g, b, a` (`GX_CH_*` 0-3) |
| 32 | TEV colours | 4 x `s16 r, g, b, a`: REG0, REG1, REG2, PREV (J3D's tevColor order) |
| 16 | konst colours | 4 x `u8 r, g, b, a`: `GX_KCOLOR0-3` |
| 20 | channel controls | 4 x (`u8 enable, matSrc, ambSrc, diffuseFn, attnFn`): COLOR0, ALPHA0, COLOR1, ALPHA1 |
| 8 | material colours | 2 x `u8 r, g, b, a`: COLOR0A0, COLOR1A1 |
| 8 | ambient colours | 2 x `u8 r, g, b, a`: COLOR0A0, COLOR1A1 |
| 5 | alpha compare | `u8 comp0, ref0, op, comp1, ref1` |
| 4 | blend | `u8 type, srcFactor, dstFactor, logicOp` |
| 4 | Z | `u8 enable, func, update, compLoc` (compLoc 1: before texturing) |
| 1 | cull | `u8` `GX_CULL_*`, or 0xFF for the material's Cull Mode |

A program is `194 + 34 * numTevStages + 4 * numIndStages + 39 * numTexGens`
bytes (`4 + 25n + 1 + 4i + 75 + 9n + 39t + 114`). With no program, the block
is the single byte 0.

A **stage** (25 bytes, all `u8`), in this order:

| # | Field | Values |
|---|---|---|
| 0 | texCoord | `GX_TEXCOORD0-7`, 0xFF none |
| 1 | texMap | `GX_TEXMAP0-7` (= texture slot), 0xFF none |
| 2 | channel | `GX_COLOR0A0` 4, `GX_COLOR1A1` 5, `GX_COLORZERO` 6, `GX_ALPHA_BUMP` 7, `GX_ALPHA_BUMPN` 8, 0xFF `GX_COLORNULL` |
| 3-6 | colorA, colorB, colorC, colorD | `GX_CC_*` 0-15 |
| 7 | colorOp | `GX_TEV_ADD` 0, `GX_TEV_SUB` 1, `GX_TEV_COMP_*` 8-15 |
| 8 | colorBias | `GX_TB_*` 0-2 |
| 9 | colorScale | `GX_CS_*` 0-3 |
| 10 | colorClamp | 0 / 1 |
| 11 | colorOut | `GX_TEVPREV` 0, `GX_TEVREG0-2` 1-3 |
| 12-15 | alphaA, alphaB, alphaC, alphaD | `GX_CA_*` 0-7 |
| 16 | alphaOp | as colorOp |
| 17 | alphaBias | `GX_TB_*` |
| 18 | alphaScale | `GX_CS_*` |
| 19 | alphaClamp | 0 / 1 |
| 20 | alphaOut | `GX_TEVPREV` 0, `GX_TEVREG0-2` 1-3 |
| 21 | kColorSel | `GX_TEV_KCSEL_*` 0x00-0x1F |
| 22 | kAlphaSel | `GX_TEV_KASEL_*` 0x00-0x1F |
| 23 | rasSwap | swap table 0-3 for the channel's colour |
| 24 | texSwap | swap table 0-3 for the texture's colour |

A **TEV stage's indirect part** (9 bytes, all `u8`, `GX_SetTevIndirect`'s
arguments). All zeroes is a direct stage (`GX_SetTevDirect`).

| # | Field | Values |
|---|---|---|
| 0 | indStage | `GX_INDTEXSTAGE0-3` |
| 1 | format | `GX_ITF_8` 0, `_5` 1, `_4` 2, `_3` 3 |
| 2 | bias | `GX_ITB_*` 0-7 |
| 3 | matrix | `GX_ITM_OFF` 0, `GX_ITM_0-2` 1-3, `GX_ITM_S0-2` 5-7, `GX_ITM_T0-2` 9-11 |
| 4 | wrapS | `GX_ITW_*` 0-6 |
| 5 | wrapT | `GX_ITW_*` 0-6 |
| 6 | addPrev | 0 / 1 |
| 7 | utcLod | 0 / 1 |
| 8 | alphaSel | `GX_ITBA_*` 0-3 |

A **texgen** (39 bytes):

| Offset | Type | Field | Values |
|---|---|---|---|
| 0 | u8 | type | `GX_TG_MTX3x4` 0, `GX_TG_MTX2x4` 1 |
| 1 | u8 | source | `GX_TG_POS` 0, `GX_TG_NRM` 1, `GX_TG_TEX0` 4, `GX_TG_TEX1` 5 |
| 2 | u8 | matrix | 0 none (`GX_IDENTITY`), 1 J3D basic SRT, 2 J3D Maya SRT, 3 the screen (type and source then ignored: 3x4 from the position) |
| 3 | f32 | scaleS | |
| 7 | f32 | scaleT | |
| 11 | f32 | rotation | degrees |
| 15 | f32 | translationS | |
| 19 | f32 | translationT | |
| 23 | f32 | centerS | basic only |
| 27 | f32 | centerT | basic only |
| 31 | f32 | scrollS | translation units a second |
| 35 | f32 | scrollT | |

A **channel control** (5 bytes): `enable` (0 / 1), `matSrc` and `ambSrc`
(`GX_SRC_REG` 0, `GX_SRC_VTX` 1), `diffuseFn` (`GX_DF_NONE` 0, `GX_DF_SIGNED`
1, `GX_DF_CLAMP` 2), `attnFn` (`GX_AF_SPEC` 0, `GX_AF_SPOT` 1, `GX_AF_NONE` 2).

### Writing it from Python

```python
import struct

def gx_program_bytes(p):
    if p is None:
        return b"\x00"
    out = bytearray(struct.pack("<BBBB", 1, len(p.stages), len(p.texgens), p.num_chans))
    for s in p.stages:
        out += struct.pack("<25B", s.tex_coord, s.tex_map, s.channel,
                           *s.color_in, s.color_op, s.color_bias, s.color_scale, s.color_clamp, s.color_out,
                           *s.alpha_in, s.alpha_op, s.alpha_bias, s.alpha_scale, s.alpha_clamp, s.alpha_out,
                           s.k_color_sel, s.k_alpha_sel, s.ras_swap, s.tex_swap)
    out += struct.pack("<B", len(p.ind_stages))
    for i in p.ind_stages:
        out += struct.pack("<4B", i.tex_coord, i.tex_map, i.scale_s, i.scale_t)
    for m in p.ind_matrices:               # always 3: (rows [[m00, m01, m02], [m10, m11, m12]], scale_exp)
        out += struct.pack("<6fb", *m.rows[0], *m.rows[1], m.scale_exp)
    for s in p.stages:                     # each stage's indirect part; all 0 for a direct stage
        out += struct.pack("<9B", s.ind_stage, s.ind_format, s.ind_bias, s.ind_matrix, s.ind_wrap_s,
                           s.ind_wrap_t, s.ind_add_prev, s.ind_utc_lod, s.ind_alpha_sel)
    for t in p.texgens:
        out += struct.pack("<BBB9f", t.type, t.source, t.matrix, t.scale_s, t.scale_t, t.rotation_deg,
                           t.trans_s, t.trans_t, t.center_s, t.center_t, t.scroll_s, t.scroll_t)
    for table in p.swap_tables:            # 4 x (r, g, b, a)
        out += struct.pack("<4B", *table)
    for color in p.tev_colors:             # REG0, REG1, REG2, PREV: (r, g, b, a) s16
        out += struct.pack("<4h", *color)
    for color in p.konst_colors:           # K0-K3
        out += struct.pack("<4B", *color)
    for c in p.chans:                      # COLOR0, ALPHA0, COLOR1, ALPHA1
        out += struct.pack("<5B", c.enable, c.mat_src, c.amb_src, c.diffuse_fn, c.attn_fn)
    for color in p.mat_colors + p.amb_colors:   # COLOR0A0, COLOR1A1 each
        out += struct.pack("<4B", *color)
    out += struct.pack("<5B", *p.alpha_compare)   # comp0, ref0, op, comp1, ref1
    out += struct.pack("<4B", *p.blend)           # type, src, dst, logic op
    out += struct.pack("<4B", *p.z)               # enable, func, update, compLoc
    out += struct.pack("<B", p.cull)              # GX_CULL_*, or 0xFF
    return bytes(out)
```

Write the asset header's version as 22 or later. Write these bytes straight
after the `mSharpSpecular` byte, the last field before the block.
