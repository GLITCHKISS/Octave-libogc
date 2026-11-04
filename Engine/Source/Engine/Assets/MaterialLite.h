#pragma once

#include "Assets/Material.h"
#include "Assets/GxProgram.h"

#include <memory>

extern const char* gShadingModelStrings[];
extern const char* gVertexColorModeStrings[];

struct MaterialLiteParams
{
    ShadingModel mShadingModel = ShadingModel::Lit;
    BlendMode mBlendMode = BlendMode::Opaque;
    VertexColorMode mVertexColorMode = VertexColorMode::Modulate;
    TextureRef mTextures[MATERIAL_LITE_MAX_TEXTURES];
    uint8_t mUvMaps[MATERIAL_LITE_MAX_TEXTURES] = { };
    TevMode mTevModes[MATERIAL_LITE_MAX_TEXTURES] = { TevMode::Replace, TevMode::Modulate, TevMode::Modulate, TevMode::Modulate };
    glm::vec2 mUvOffsets[MAX_UV_MAPS] = { { 0.0f, 0.0f }, {0.0f, 0.0f} };
    glm::vec2 mUvScales[MAX_UV_MAPS] = { { 1.0f, 1.0f }, {1.0f, 1.0f} };
    glm::vec4 mColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    glm::vec4 mFresnelColor = { 1.0f, 0.0f, 0.0f, 0.0f };
    float mEmission = 0.0f;
    float mWrapLighting = 0.0f;
    float mFresnelPower = 1.0f;
    float mSpecular = 0.0f;
    uint32_t mToonSteps = 2;
    float mOpacity = 1.0f;
    float mMaskCutoff = 0.5f;
    // Kept only where alpha is at most mAlphaKeepBelow or at least mAlphaKeepAbove, the rest discarded (the GameCube's
    // alpha compare, GX_LEQUAL OR GX_GEQUAL): 1, 1 keeps all of it. The band between drops out, so what's kept has
    // a crisp edge. Below over above turns it round: only the band between is kept, and an additive material adds
    // its colour flat there (the alpha only chooses where) -- a copy of a banded material that fills its gaps.
    float mAlphaKeepBelow = 1.0f;
    float mAlphaKeepAbove = 1.0f;
    // Its textures sampled log2(picture height / 548) mip levels smaller on top of their own bias: hand-drawn mip
    // levels made for the GameCube's 640 x 548 picture land where they were meant to, whatever the window.
    bool mMipBiasFollowsHeight = false;
    // Its Fresnel term as its opacity, not added colour: see-through face on, solid edge on (as water: the flatter
    // it's seen, the less of what's under it shows).
    bool mFresnelAlpha = false;
    // On the GameCube, its specular per pixel: the PC renderer's Phong lobe for its shininess, baked into a
    // small texture placed by the surface normal, instead of GX's per-vertex specular channel (which on a
    // low-poly mesh smears the highlight over whole triangles). The PC renderer is per pixel already.
    bool mSharpSpecular = false;
    float mShininess = 32.0f;
    int32_t mSortPriority = 0;
    uint32_t mNumTextures = 1;
    bool mDisableDepthTest = false;
    bool mFresnelEnabled = false;
    bool mApplyFog = true;
    CullMode mCullMode = CullMode::Back;
    // A raw GX program (GxProgram.h), or none: on the GameCube its GX state is the whole material. Shared by the
    // materials copied from this one (New(src), SetLiteParams): nothing edits a program once it's loaded.
    std::shared_ptr<const GxProgram> mGxProgram;
};

// THE GX PROGRAM IN THE STREAM (ASSET_VERSION_GX_PROGRAM on), after mSharpSpecular, for a tool that writes MaterialLite
// assets itself. Little-endian, as all of an asset (the GameCube's reader swaps); u8 = 1 byte, s16 = 2, f32 = 4 (IEEE);
// no padding or alignment anywhere. Every enum is GX's own number (GxProgram.h).
//
//   u8   present                 0: no program, and nothing more follows. 1: the rest follows.
//   u8   numTevStages            1-16
//   u8   numTexGens              0-8
//   u8   numChans                0-2 (GX_SetNumChans; 0 is drawn as 1)
//   numTevStages x 25 bytes, a stage each (GxProgramStage), in this order:
//        u8 texCoord, u8 texMap, u8 channel                                   (0xFF: none)
//        u8 colorA, colorB, colorC, colorD, colorOp, colorBias, colorScale, colorClamp, colorOut
//        u8 alphaA, alphaB, alphaC, alphaD, alphaOp, alphaBias, alphaScale, alphaClamp, alphaOut
//        u8 kColorSel, u8 kAlphaSel, u8 rasSwap, u8 texSwap
//   u8   numIndStages            0-4 (indirect texture stages)
//   numIndStages x 4 bytes        u8 texCoord, u8 texMap, u8 scaleS, u8 scaleT (GX_ITS_*)
//   3 x 25 bytes, the indirect matrices GX_ITM_0, 1, 2 (always all three):
//        f32 m00, m01, m02, m10, m11, m12 (GX_SetIndTexMatrix's offset_mtx[2][3], row by row), s8 scaleExp
//   numTevStages x 9 bytes, each TEV stage's indirect part (GX_SetTevIndirect; all 0: a direct stage):
//        u8 indStage, format, bias, matrix, wrapS, wrapT, addPrev, utcLod, alphaSel
//   numTexGens x 39 bytes, a texgen each (GxProgramTexGen; texgen i is GX_TEXCOORD0 + i, its matrix GX_TEXMTX0 + i x 3):
//        u8 type, u8 source, u8 matrix (0 none / GX_IDENTITY, 1 J3D basic SRT, 2 J3D Maya SRT, 3 the screen)
//        f32 scaleS, scaleT, rotation (degrees), translationS, translationT, centerS, centerT, scrollS, scrollT
//   16 bytes  swap tables         4 tables (GX_TEV_SWAP0-3) x u8 r, g, b, a (GX_CH_*)
//   32 bytes  TEV colours         4 x s16 r, g, b, a: GX_TEVREG0, GX_TEVREG1, GX_TEVREG2, GX_TEVPREV (J3D's order)
//   16 bytes  konst colours       4 x u8 r, g, b, a: GX_KCOLOR0-3
//   20 bytes  channel controls    4 x (u8 enable, matSrc, ambSrc, diffuseFn, attnFn): COLOR0, ALPHA0, COLOR1, ALPHA1
//    8 bytes  material colours    2 x u8 r, g, b, a: COLOR0A0, COLOR1A1
//    8 bytes  ambient colours     2 x u8 r, g, b, a: COLOR0A0, COLOR1A1
//    5 bytes  alpha compare       u8 comp0, ref0, op, comp1, ref1
//    4 bytes  blend               u8 type, srcFactor, dstFactor, logicOp
//    4 bytes  Z                   u8 enable, func, update, compLoc (1: before texturing)
//    1 byte   cull                u8 GX_CULL_*, or 0xFF for the material's own Cull Mode
//
// So a program is 194 + 34 x numTevStages + 4 x numIndStages + 39 x numTexGens bytes (4 + 25 n + 1 + 4 i + 75 + 9 n
// + 39 t + 114); with none, the one byte 0.

class MaterialLite : public Material
{
public:

    DECLARE_ASSET(MaterialLite, Material);

    static MaterialLite* New(Material* src = nullptr);

    MaterialLite();
    ~MaterialLite();

    virtual void LoadStream(Stream& stream, Platform platform) override;
    virtual void SaveStream(Stream& stream, Platform platform) override;
    virtual void Create() override;
    virtual void Destroy() override;
    virtual bool Import(const std::string& path, ImportOptions* options) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;
    virtual glm::vec4 GetTypeColor() override;
    virtual const char* GetTypeName() override;
    virtual bool DrawCustomProperty(Property& prop) override;

    virtual bool IsLite() const override;

    const MaterialLiteParams& GetLiteParams() const;
    void SetLiteParams(const MaterialLiteParams& params);

    void SetTexture(uint32_t slot, Texture* texture);
    Texture* GetTexture(uint32_t slot);

    ShadingModel GetShadingModel() const;
    void SetShadingModel(ShadingModel shadingModel);

    virtual BlendMode GetBlendMode() const override;
    void SetBlendMode(BlendMode blendMode);

    VertexColorMode GetVertexColorMode() const;
    void SetVertexColorMode(VertexColorMode mode);

    glm::vec2 GetUvOffset(int32_t uvIndex = 0) const;
    void SetUvOffset(glm::vec2 offset, int32_t uvIndex = 0);

    glm::vec2 GetUvScale(int32_t uvIndex = 0) const;
    void SetUvScale(glm::vec2 scale, int32_t uvIndex = 0);

    glm::vec4 GetColor() const;
    void SetColor(const glm::vec4& color);

    glm::vec4 GetFresnelColor() const;
    void SetFresnelColor(const glm::vec4& color);

    float GetEmission() const;
    void SetEmission(float emission);

    float GetWrapLighting() const;
    void SetWrapLighting(float wrapLighting);

    float GetFresnelPower() const;
    void SetFresnelPower(float power);

    float GetSpecular() const;
    void SetSpecular(float specular);

    uint32_t GetToonSteps() const;
    void SetToonSteps(uint32_t steps);

    float GetOpacity() const;
    void SetOpacity(float opacity);

    virtual float GetMaskCutoff() const override;
    void SetMaskCutoff(float cutoff);
    float GetAlphaKeepBelow() const { return mLiteParams.mAlphaKeepBelow; }
    float GetAlphaKeepAbove() const { return mLiteParams.mAlphaKeepAbove; }
    bool HasAlphaBand() const { return mLiteParams.mAlphaKeepBelow < 1.0f; }
    bool MipBiasFollowsHeight() const { return mLiteParams.mMipBiasFollowsHeight; }
    bool IsFresnelAlpha() const { return mLiteParams.mFresnelAlpha; }
    bool IsSharpSpecular() const { return mLiteParams.mSharpSpecular; }
    bool HasGxProgram() const { return mLiteParams.mGxProgram != nullptr; }
    const GxProgram* GetGxProgram() const { return mLiteParams.mGxProgram.get(); }

    float GetShininess() const;
    void SetShininess(float shininess);

    virtual int32_t GetSortPriority() const override;
    void SetSortPriority(int32_t priority);

    virtual bool IsDepthTestDisabled() const override;
    void SetDepthTestDisabled(bool depthTest);

    bool IsFresnelEnabled() const;
    void SetFresnelEnabled(bool enable);

    virtual bool ShouldApplyFog() const override;
    void SetApplyFog(bool applyFog);

    virtual CullMode GetCullMode() const override;
    void SetCullMode(CullMode cullMode);

    uint32_t GetUvMap(uint32_t textureSlot);
    void SetUvMap(uint32_t textureSlot, uint32_t uvMapIndex);

    TevMode GetTevMode(uint32_t textureSlot);
    void SetTevMode(uint32_t textureSlot, TevMode mode);

    // The texture slot that warps the others (TevMode::Warp, see Constants.h), or -1 when there
    // is none or its strength is not above 0. A later Warp slot is ignored (treated as Pass).
    int32_t GetWarpSlot();
    // The warp strength shares the Emission value (see MATERIAL_LITE_WARP_MIN_SLOT in Constants.h).
    float GetWarpStrength() const { return GetEmission(); }
    void SetWarpStrength(float strength) { SetEmission(strength); }

    // Screen refraction (GameCube and Vulkan): texture slot 0 shows what was drawn behind the surface
    // -- a copy of the frame taken before the translucent draws -- at each pixel's own place on the
    // screen, warped by the Warp slot if there is one. Set at runtime, not saved with the asset. The
    // 3DS doesn't draw the material (it has no copy of the frame).
    bool IsRefract() const { return mRefract; }
    void SetRefract(bool refract) { mRefract = refract; }

    static bool HandlePropChange(Datum* datum, uint32_t index, const void* newValue);

protected:

    // Properties
    MaterialLiteParams mLiteParams;
    bool mRefract = false;
};
