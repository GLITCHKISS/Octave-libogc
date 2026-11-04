#pragma once

#include <stdint.h>
#include <string.h>
#include <math.h>

// A RAW GX PROGRAM (MaterialLite, GameCube only: the other renderers draw the material by its own settings, as ever).
// An optional block of GX state, set as it stands when the material is bound, in place of everything the material's
// own settings would make: the TEV stages, the texgens and their matrices, the colour channels, the TEV and konst
// registers, the alpha compare, blend and Z mode of a material from another GameCube game's J3D data (BMD/BDL, MAT3),
// so it draws as that game drew it. Every value is the GX enum's own number (libogc's gx.h; the same as Nintendo's
// SDK and J3D's data), so a tool copies them straight across. Texture map n is the material's texture slot n (0-3:
// set Num Textures to cover the ones it reads; maps 4-7 read the white texture). The stream's layout is in
// MaterialLite.h and Documentation/Info/GxProgram.md.

#define GX_PROGRAM_MAX_TEV_STAGES 16
#define GX_PROGRAM_MAX_TEXGENS 8
#define GX_PROGRAM_MAX_IND_STAGES 4
#define GX_PROGRAM_IND_MATRICES 3
#define GX_PROGRAM_NONE 0xFF

// A TEV stage's indirect texturing, as GX_SetTevIndirect takes it. All 0 is a direct stage (GX_SetTevDirect:
// GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_NONE, GX_ITM_OFF, GX_ITW_OFF twice, no add, no LOD, GX_ITBA_OFF).
struct GxProgramTevInd
{
    uint8_t mIndStage = 0;                   // (GX_INDTEXSTAGE0-3)
    uint8_t mFormat = 0;                     // (GX_ITF_8 0, _5, _4, _3)
    uint8_t mBias = 0;                       // (GX_ITB_*)
    uint8_t mMatrix = 0;                     // (GX_ITM_OFF 0, GX_ITM_0-2 1-3, GX_ITM_S0-2 5-7, GX_ITM_T0-2 9-11)
    uint8_t mWrapS = 0;                      // (GX_ITW_*)
    uint8_t mWrapT = 0;
    uint8_t mAddPrev = 0;
    uint8_t mUtcLod = 0;
    uint8_t mAlphaSel = 0;                   // (GX_ITBA_*)
};

// One TEV stage, as GX_SetTevOrder, GX_SetTevColorIn/Op, GX_SetTevAlphaIn/Op, GX_SetTevKColorSel/KAlphaSel,
// GX_SetTevSwapMode and GX_SetTevIndirect take it (the indirect part a section of its own in the stream).
struct GxProgramStage
{
    uint8_t mTexCoord = GX_PROGRAM_NONE;     // (GX_TEXCOORD0-7; 0xFF GX_TEXCOORDNULL)
    uint8_t mTexMap = GX_PROGRAM_NONE;       // (GX_TEXMAP0-7, the material's texture slot n; 0xFF GX_TEXMAP_NULL)
    uint8_t mChannel = GX_PROGRAM_NONE;      // (GX_COLOR0A0 4, GX_COLOR1A1 5, GX_COLORZERO 6...; 0xFF GX_COLORNULL)
    uint8_t mColorIn[4] = { 15, 15, 15, 0 }; // (a, b, c, d: GX_CC_*; d + ((1 - c) a + c b), the default CPREV)
    uint8_t mColorOp = 0;                    // (GX_TEV_ADD 0, GX_TEV_SUB 1, a GX_TEV_COMP_* 8-15)
    uint8_t mColorBias = 0;                  // (GX_TB_*)
    uint8_t mColorScale = 0;                 // (GX_CS_*)
    uint8_t mColorClamp = 1;
    uint8_t mColorOut = 0;                   // (GX_TEVPREV 0, GX_TEVREG0-2 1-3)
    uint8_t mAlphaIn[4] = { 7, 7, 7, 0 };    // (GX_CA_*; the default APREV)
    uint8_t mAlphaOp = 0;
    uint8_t mAlphaBias = 0;
    uint8_t mAlphaScale = 0;
    uint8_t mAlphaClamp = 1;
    uint8_t mAlphaOut = 0;
    uint8_t mKColorSel = 0;                  // (GX_TEV_KCSEL_*: what GX_CC_KONST reads)
    uint8_t mKAlphaSel = 0;                  // (GX_TEV_KASEL_*: what GX_CA_KONST reads)
    uint8_t mRasSwap = 0;                    // (GX_TEV_SWAP0-3: the swap table for the channel's colour)
    uint8_t mTexSwap = 0;                    // (... and for the texture's)
    GxProgramTevInd mInd;
};

// An indirect texture stage, as GX_SetIndTexOrder and GX_SetIndTexCoordScale take it
struct GxProgramIndStage
{
    uint8_t mTexCoord = 0;                   // (GX_TEXCOORD0-7)
    uint8_t mTexMap = 0;                     // (GX_TEXMAP0-7, the material's texture slot n)
    uint8_t mScaleS = 0;                     // (GX_ITS_1 0 ... GX_ITS_256 8)
    uint8_t mScaleT = 0;
};

// An indirect matrix (GX_ITM_0-2), as GX_SetIndTexMatrix takes it and J3D keeps it: 2 x 3, its entries under 1,
// times 2 ^ mScaleExp
struct GxProgramIndMtx
{
    float mMtx[2][3] = { { 0.5f, 0.0f, 0.0f }, { 0.0f, 0.5f, 0.0f } };
    int8_t mScaleExp = 0;
};

// mMatrix: no matrix (GX_IDENTITY: the coordinates as they come), J3D's 'basic' texture SRT (about mCenter), or its
// Maya one (about 0.5, 0.5, as J3D has it: no centre) -- J3D's texture matrix info, its top bit. Or the screen: the
// pixel's own place on it, from the position through the refraction's matrix (GX_TEXMTX9, GxLoadScreenTexMtx, 3x4;
// the SRT unused) -- with SetRefract on, map 0 is then the frame copied before the translucent draws, as a J3D
// material sampling an EFB copy of the frame (a heat haze's warp) reads it.
#define GX_PROGRAM_MTX_NONE 0
#define GX_PROGRAM_MTX_BASIC 1
#define GX_PROGRAM_MTX_MAYA 2
#define GX_PROGRAM_MTX_SCREEN 3

// One texture coordinate, as GX_SetTexCoordGen takes it, its matrix in GX_TEXMTX0 + its index x 3 (built by
// GxProgramTexMtx).
struct GxProgramTexGen
{
    uint8_t mType = 1;                       // (GX_TG_MTX2x4 1, GX_TG_MTX3x4 0)
    uint8_t mSource = 4;                     // (GX_TG_TEX0 4: the mesh's UV 0, GX_TG_TEX1 5: its UV 1; GX_TG_POS 0,
                                             //  GX_TG_NRM 1: the vertex's own, in the mesh's space)
    uint8_t mMatrix = GX_PROGRAM_MTX_BASIC;
    float mScale[2] = { 1.0f, 1.0f };
    float mRotation = 0.0f;                  // (degrees: J3D's s16 / 0x7FFF x 180)
    float mTranslation[2] = { 0.0f, 0.0f };
    float mCenter[2] = { 0.5f, 0.5f };       // (the basic SRT's only)
    float mScroll[2] = { 0.0f, 0.0f };       // (translation units a second, added with the game's time)
};

// One of the four channel controls, as GX_SetChanCtrl takes it. Lit (mEnable 1): by the lights the draw's own
// lighting gives it (SetupLightMask: the material's Shading Model must not be Unlit), not J3D's light mask.
struct GxProgramChan
{
    uint8_t mEnable = 0;
    uint8_t mMatSrc = 0;                     // (GX_SRC_REG 0, GX_SRC_VTX 1: the mesh's vertex colour; REG without one)
    uint8_t mAmbSrc = 0;
    uint8_t mDiffuseFn = 0;                  // (GX_DF_NONE 0, GX_DF_SIGNED 1, GX_DF_CLAMP 2)
    uint8_t mAttnFn = 2;                     // (GX_AF_SPEC 0, GX_AF_SPOT 1, GX_AF_NONE 2)
};

// The channels: how many are rasterised (GX_SetNumChans; at least one is, as a draw with no channel and no texgen
// hangs the GPU) and COLOR0, ALPHA0, COLOR1, ALPHA1's controls (J3D's colour channel order)
struct GxProgramChannels
{
    uint8_t mNumChans = 1;
    GxProgramChan mChans[4];

    bool operator==(const GxProgramChannels& other) const
    {
        return memcmp(this, &other, sizeof(GxProgramChannels)) == 0;   // (bytes only: no padding)
    }
    bool operator!=(const GxProgramChannels& other) const { return !operator==(other); }
};

struct GxProgram
{
    uint8_t mNumTevStages = 1;
    uint8_t mNumTexGens = 0;
    uint8_t mNumIndStages = 0;
    GxProgramStage mStages[GX_PROGRAM_MAX_TEV_STAGES];
    GxProgramIndStage mIndStages[GX_PROGRAM_MAX_IND_STAGES];
    GxProgramIndMtx mIndMtx[GX_PROGRAM_IND_MATRICES];
    GxProgramTexGen mTexGens[GX_PROGRAM_MAX_TEXGENS];
    uint8_t mSwapTables[4][4] = { { 0, 1, 2, 3 }, { 0, 1, 2, 3 }, { 0, 1, 2, 3 }, { 0, 1, 2, 3 } };   // (GX_CH_* for r, g, b, a)
    int16_t mTevColors[4][4] = { };          // (GX_TEVREG0, 1, 2 and GX_TEVPREV -- J3D's order -- signed 10-bit RGBA)
    uint8_t mKonstColors[4][4] = { };        // (GX_KCOLOR0-3, RGBA)
    GxProgramChannels mChannels;
    uint8_t mMatColors[2][4] = { { 255, 255, 255, 255 }, { 255, 255, 255, 255 } };   // (COLOR0A0, COLOR1A1)
    uint8_t mAmbColors[2][4] = { { 0, 0, 0, 255 }, { 0, 0, 0, 255 } };
    uint8_t mAlphaComp0 = 7;                 // (GX_ALWAYS)
    uint8_t mAlphaRef0 = 0;
    uint8_t mAlphaOp = 1;                    // (GX_AOP_OR)
    uint8_t mAlphaComp1 = 7;
    uint8_t mAlphaRef1 = 0;
    uint8_t mBlendType = 0;                  // (GX_BM_NONE)
    uint8_t mBlendSrc = 1;                   // (GX_BL_ONE)
    uint8_t mBlendDst = 0;                   // (GX_BL_ZERO)
    uint8_t mLogicOp = 0;                    // (GX_LO_CLEAR)
    uint8_t mZEnable = 1;
    uint8_t mZFunc = 3;                      // (GX_LEQUAL)
    uint8_t mZUpdate = 1;
    uint8_t mZCompLoc = 1;                   // (1: Z tested before texturing; 0: after, as an alpha compare needs)
    uint8_t mCullMode = GX_PROGRAM_NONE;     // (GX_CULL_*, as given; 0xFF the material's own Cull Mode)
};

// x - 2 floor(x / 2): a scroll moved on by whole periods, which a repeating texture (period 1) and a mirrored one
// (period 2) both draw the same -- the floats stay small however long the game runs. In double: rate x time x scale
// reaches the tens of thousands in an hour, where a float's step is a few thousandths of the texture.
inline float GxProgramWrapScroll(double x)
{
    return float(x - 2.0 * floor(x * 0.5));
}

// A texgen's texture matrix, `time` seconds into the game: rows S and T, the third row 0 0 1 0 (a 3x4 texgen's q),
// as J3D builds a texture SRT for map mode 0 (no mapping) -- noclip's calcTexMtx_Basic and calcTexMtx_Maya
// (src/Common/JSYSTEM/J3D/J3DLoader.ts), with theta = mRotation in radians and M = [[sS cos, -sS sin], [sT sin, sT cos]]
// (the scale after the rotation):
//   basic: [s' t'] = M ([s t] - center) + center + translation
//   Maya:  M' = [[sS cos, sS sin], [-sT sin, sT cos]], its translation
//          (sS (-0.5 cos - (0.5 sin - 0.5) - tS), sT (-0.5 cos + (0.5 sin - 0.5) + tT) + 1)
// The scroll's rate x time is added to the translation the same way (into column 3 as the SRT puts it there: for
// Maya, times -sS and sT), wrapped by GxProgramWrapScroll. Column 2 is 0: GX gives a texture coordinate's input as
// (s, t, 1, 1), so the translation lives in column 3 alone.
inline void GxProgramTexMtx(const GxProgramTexGen& texGen, float time, float out[3][4])
{
    const float theta = texGen.mRotation * (3.14159265358979f / 180.0f);
    const float sinR = sinf(theta);
    const float cosR = cosf(theta);
    const float sS = texGen.mScale[0];
    const float sT = texGen.mScale[1];
    const double scrollS = double(texGen.mScroll[0]) * double(time);
    const double scrollT = double(texGen.mScroll[1]) * double(time);

    memset(out, 0, sizeof(float) * 12);
    out[2][2] = 1.0f;

    if (texGen.mMatrix == GX_PROGRAM_MTX_MAYA)
    {
        out[0][0] = sS * cosR;
        out[0][1] = sS * sinR;
        out[0][3] = sS * ((-0.5f * cosR) - (0.5f * sinR - 0.5f) - texGen.mTranslation[0]);
        out[1][0] = sT * -sinR;
        out[1][1] = sT * cosR;
        out[1][3] = sT * ((-0.5f * cosR) + (0.5f * sinR - 0.5f) + texGen.mTranslation[1]) + 1.0f;
        out[0][3] += GxProgramWrapScroll(-double(sS) * scrollS);
        out[1][3] += GxProgramWrapScroll(double(sT) * scrollT);
    }
    else
    {
        const float cS = texGen.mCenter[0];
        const float cT = texGen.mCenter[1];
        out[0][0] = sS * cosR;
        out[0][1] = sS * -sinR;
        out[0][3] = texGen.mTranslation[0] + cS - (out[0][0] * cS + out[0][1] * cT) + GxProgramWrapScroll(scrollS);
        out[1][0] = sT * sinR;
        out[1][1] = sT * cosR;
        out[1][3] = texGen.mTranslation[1] + cT - (out[1][0] * cS + out[1][1] * cT) + GxProgramWrapScroll(scrollT);
    }
}
