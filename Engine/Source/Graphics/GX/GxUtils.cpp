#if API_GX

#include "Graphics/GX/GxUtils.h"
#include "EngineFeatures.h"
#include "Graphics/GX/GxTypes.h"
#include "Graphics/GraphicsTypes.h"

#include "World.h"
#include "Engine.h"
#include "Renderer.h"
#include "Log.h"
#include "Vertex.h"
#include "Nodes/Widgets/Widget.h"
#include "Assets/SkeletalMesh.h"

#include "Nodes/3D/Camera3d.h"
#include "Nodes/3D/DirectionalLight3d.h"
#include "Nodes/3D/PointLight3d.h"
#include "Nodes/3D/SkeletalMesh3d.h"

#include <malloc.h>
#include <math.h>
#include <algorithm>

extern GxContext gGxContext;

void SetupLights()
{
    Camera3D* cameraComp = gGxContext.mWorld->GetActiveCamera();
    if (cameraComp == nullptr)
    {
        return;
    }

    gGxContext.mLighting.mLightMask = 0;

    // Setup lights
    const std::vector<LightData>& lightArray = Renderer::Get()->GetLightData();

    gGxContext.mSceneLightMask = 0;
    gGxContext.mSceneNumLights = 0;
    gGxContext.mHasSpecLight = false;

    // Light0 is reserved for directional light, in the future we might allow multiple dir lights.
    for (uint32_t i = 0; i < lightArray.size() && i < MAX_LIGHTS_PER_DRAW; ++i)
    {
        const LightData& lightData = lightArray[i];
        GXLightObj gxLight;

        glm::vec3 lightPosWS = lightData.mPosition;
        if (lightData.mType == LightType::Directional)
        {
            lightPosWS = cameraComp->GetWorldPosition() + -lightData.mDirection * 10000.0f;
        }
        glm::vec4 lightPosVS = cameraComp->GetViewMatrix() * glm::vec4(lightPosWS, 1.0f);

        glm::vec4 lightColor = lightData.mColor * gGxContext.mInvColorScale;
        lightColor *= lightData.mIntensity;
        lightColor.a = 1.0f;
        lightColor = glm::clamp(lightColor, 0.0f, 1.0f);
        GXColor gxLightColor = { uint8_t(lightColor.r * 255.0f),
                                    uint8_t(lightColor.g * 255.0f),
                                    uint8_t(lightColor.b * 255.0f),
                                    uint8_t(lightColor.a * 255.0f) };

        GX_InitLightPos(&gxLight, lightPosVS.x, lightPosVS.y, lightPosVS.z);
        GX_InitLightColor(&gxLight, gxLightColor);
        GX_InitLightSpot(&gxLight, 0.0f, GX_SP_OFF);

        if (lightData.mType == LightType::Directional)
        {
            GX_InitLightDistAttn(&gxLight, 0.0f, 1.0f, GX_DA_OFF);
        }
        else
        {
            GX_InitLightDistAttn(&gxLight, lightData.mRadius, 0.25f, GX_DA_MEDIUM);
        }

        if (lightData.mType == LightType::Directional && !gGxContext.mHasSpecLight)
        {
            glm::vec4 dirVS = cameraComp->GetViewMatrix() * glm::vec4(lightData.mDirection, 0.0f);
            gGxContext.mSpecDirVS = glm::normalize(glm::vec3(dirVS));
            gGxContext.mSpecColor = gxLightColor;
            gGxContext.mSpecHalfVS = glm::normalize(-gGxContext.mSpecDirVS + glm::vec3(0.0f, 0.0f, 1.0f));
            gGxContext.mHasSpecLight = true;
        }

        GX_LoadLightObj(&gxLight, GX_LIGHT0 << gGxContext.mSceneNumLights);
        gGxContext.mSceneLightMask |= (GX_LIGHT0 << gGxContext.mSceneNumLights);
        gGxContext.mLightData[gGxContext.mSceneNumLights] = lightData;
        gGxContext.mSceneNumLights++;
    }
}

void SetupLightMask(ShadingModel shadingModel, uint8_t lightingChannels, bool useBakedLight)
{
    uint8_t lightMask = 0;

    if (shadingModel != ShadingModel::Unlit)
    {
        // This step needs to determine which light types should affect the primitive being drawn.
        // For instance, an All domain light should not affect a static mesh that has baked lighting.
        for (uint32_t i = 0; i < gGxContext.mSceneNumLights; ++i)
        {
            const LightData& lightData = gGxContext.mLightData[i];

            if ((lightData.mDomain == LightingDomain::All) && useBakedLight)
            {
                continue;
            }

            if ((lightData.mLightingChannels & lightingChannels) == 0)
            {
                continue;
            }

            // In the future, use distance checking to determine which lights should affect a draw.
            // This is slightly tough right now because of the weird attenuation functions.
            // There isn't a point where the light intensity reaches 0 exactly.

            lightMask |= (GX_LIGHT0 << i);
        }
    }

    gGxContext.mLighting.mLightMask = lightMask;
}

void SetupLightingChannels()
{
    static LightingState prevState(false, false, 0, 0, 0, 0);
    LightingState& curState = gGxContext.mLighting;

    if (curState != prevState)
    {
        // If we are using vertex color modulation, then we use the first channel for that
        // since the vertex color can't go as input in the COLOR1A1 unless you have two vertex color attributes.
        GX_SetNumChans((curState.mColorChannel || curState.mSpecularMask != 0) ? 2 : 1);

        if (curState.mColorChannel)
        {
            GX_SetChanCtrl(
                GX_COLOR0A0,
                false,
                GX_SRC_VTX,
                GX_SRC_VTX,
                0,
                GX_DF_NONE,
                GX_AF_NONE);
        }
        else if (curState.mSpecularMask != 0)
        {
            // The specular: the material's specular colour (the register) lit by the specular light
            // alone, attenuated by the angle to its half vector (GX_AF_SPEC); no ambient.
            GX_SetChanCtrl(GX_COLOR1, GX_ENABLE, GX_SRC_REG, GX_SRC_REG, curState.mSpecularMask, GX_DF_NONE, GX_AF_SPEC);
            GX_SetChanCtrl(GX_ALPHA1, false, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
        }
        else
        {
            // I was crashing without "zeroing" the second channel even
            // when it wasn't in use.
            GX_SetChanCtrl(
                GX_COLOR1A1,
                false,
                GX_SRC_VTX,
                GX_SRC_VTX,
                0,
                GX_DF_NONE,
                GX_AF_NONE);
        }

        GX_SetChanCtrl(
            curState.mColorChannel ? GX_COLOR1A1 : GX_COLOR0A0,
            curState.mEnabled,
            GX_SRC_REG,
            curState.mMaterialSrc,
            curState.mLightMask,
            curState.mDiffuseFunc,
            curState.mAttenuationFunc);

        prevState = curState;
    }
}

void PrepareForwardRendering()
{
    GX_SetCullMode(GX_CULL_FRONT);
}

void PrepareUiRendering()
{
    // Widgets set up their own TEV stages; none of them may read a warp material's indirect stage.
    GxResetIndirect();

    GX_SetCullMode(GX_CULL_NONE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);

    // Setup matrices
    glm::vec2 res = Renderer::Get()->GetScreenResolution();
    Mtx44 projectionUI;
    guOrtho(projectionUI, 0, res.y, 0, res.x, -100.0f, 100.0f);
    GX_LoadProjectionMtx(projectionUI, GX_ORTHOGRAPHIC);

    Mtx modelViewUI;
    guMtxIdentity(modelViewUI);
    GX_LoadPosMtxImm(modelViewUI, GX_PNMTX0);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);

    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    // Probably not needed since lighting is disabled, but set material color to white.
    GX_SetChanMatColor(GX_COLOR0A0, { 255, 255, 255, 255 });

    gGxContext.mLighting.mColorChannel = false;
    gGxContext.mLighting.mEnabled = false;
    gGxContext.mLighting.mSpecularMask = 0;
    gGxContext.mLighting.mMaterialSrc = GX_SRC_VTX;
}

#if OCT_SKELETAL
bool IsCpuSkinningRequired(SkeletalMesh3D* component)
{
    if (component->GetSkeletalMesh() == nullptr)
    {
        return false;
    }
    else
    {
        return component->GetSkeletalMesh()->GetNumBones() > MAX_GPU_BONES;
    }
}
#endif

// Sharp specular: the PC renderer's Phong lobe, pow(max(V.R, 0), shininess), as a function of the angle t between
// the normal and the light's half vector (V.R = cos 2t), baked into a texture -- its middle the half vector, its
// edge t = 45 degrees, where the lobe is 0 -- placed by the view-space normal (LoadSpecTexMtx). One a shininess.
static const float kSpecTexScale = 0.5f / 0.78539816f;   // (texture units a radian of t)
struct SpecLobeTex { void* mData = nullptr; GXTexObj mObj; };
static SpecLobeTex sSpecLobes[16];
static int32_t sSpecLobeKeys[16];
static uint32_t sNumSpecLobes = 0;

static GXTexObj* GetSpecLobeTex(float shininess)
{
    const int32_t key = int32_t(shininess * 4.0f + 0.5f);
    for (uint32_t i = 0; i < sNumSpecLobes; ++i)
        if (sSpecLobeKeys[i] == key)
            return &sSpecLobes[i].mObj;
    if (sNumSpecLobes >= 16)
        return nullptr;

    const uint32_t size = 128;   // I8: tiles of 8 x 4
    uint8_t* data = (uint8_t*)memalign(32, size * size);
    if (data == nullptr)
        return nullptr;
    uint8_t* out = data;
    for (uint32_t ty = 0; ty < size; ty += 4)
        for (uint32_t tx = 0; tx < size; tx += 8)
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 8; ++x)
                {
                    const float u = (float(tx + x) + 0.5f) / size - 0.5f;
                    const float v = (float(ty + y) + 0.5f) / size - 0.5f;
                    const float t = sqrtf(u * u + v * v) / kSpecTexScale;
                    const float c = cosf(2.0f * t);
                    const float lobe = (t < 0.78539816f && c > 0.0f) ? powf(c, shininess) : 0.0f;
                    *out++ = uint8_t(glm::clamp(lobe, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
    DCFlushRange(data, size * size);

    SpecLobeTex& lobeTex = sSpecLobes[sNumSpecLobes];
    lobeTex.mData = data;
    GX_InitTexObj(&lobeTex.mObj, data, size, size, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&lobeTex.mObj, GX_LINEAR, GX_LINEAR);
    sSpecLobeKeys[sNumSpecLobes] = key;
    sNumSpecLobes++;
    return &lobeTex.mObj;
}

// The lobe texture's coordinates from the normal: (n_view - h) * scale + 0.5, h the half vector.
static void LoadSpecTexMtx(const Mtx normalMtx, float k)
{
    const glm::vec3& h = gGxContext.mSpecHalfVS;
    Mtx m;
    for (uint32_t c = 0; c < 3; ++c)
    {
        m[0][c] = kSpecTexScale * k * normalMtx[0][c];
        m[1][c] = kSpecTexScale * k * normalMtx[1][c];
        m[2][c] = 0.0f;
    }
    m[0][3] = 0.5f - kSpecTexScale * h.x;
    m[1][3] = 0.5f - kSpecTexScale * h.y;
    m[2][3] = 1.0f;
    GX_LoadTexMtxImm(m, GX_TEXMTX5, GX_TG_MTX3x4);
}

void LoadEnvTexMtx(const Mtx normalMtx)
{
    if (!gGxContext.mEnvMapBound && !gGxContext.mSpecTexBound)
    {
        return;
    }

    // The texgen reads the raw (object space) normal, so the matrix takes it to view space first.
    // The normal matrix is the inverse transpose of the model-view: an object scaled by s gives
    // rows 1/s long. Its determinant is then 1/s^3, so scale back by the cube root to keep the
    // coordinates on the matcap. (Non-uniform scale cannot be undone per object; GX has no
    // per-vertex normalize without dual texgen, so such an object's mapping is a little off.)
    const float det =
        normalMtx[0][0] * (normalMtx[1][1] * normalMtx[2][2] - normalMtx[1][2] * normalMtx[2][1]) -
        normalMtx[0][1] * (normalMtx[1][0] * normalMtx[2][2] - normalMtx[1][2] * normalMtx[2][0]) +
        normalMtx[0][2] * (normalMtx[1][0] * normalMtx[2][1] - normalMtx[1][1] * normalMtx[2][0]);
    const float absDet = fabsf(det);
    const float k = (absDet > 1e-12f) ? 1.0f / cbrtf(absDet) : 1.0f;

    if (gGxContext.mSpecTexBound)
    {
        LoadSpecTexMtx(normalMtx, k);
    }
    if (!gGxContext.mEnvMapBound)
    {
        return;
    }

    // u = n_view.x * 0.5 + 0.5, v = -n_view.y * 0.5 + 0.5 (see UV_MAP_ENVIRONMENT); q = 1, as a
    // 3x4 texgen divides by its third row.
    Mtx env;
    for (uint32_t c = 0; c < 3; ++c)
    {
        env[0][c] = 0.5f * k * normalMtx[0][c];
        env[1][c] = -0.5f * k * normalMtx[1][c];
        env[2][c] = 0.0f;
    }
    env[0][3] = 0.5f;
    env[1][3] = 0.5f;
    env[2][3] = 1.0f;

    GX_LoadTexMtxImm(env, GX_TEXMTX2, GX_TG_MTX3x4);
}

// The texgen for a texture slot's coordinates, by its UV map.
static void SetupSlotTexGen(uint32_t texIdx, uint32_t uvMap, bool hasNormals)
{
    if (uvMap == UV_MAP_ENVIRONMENT && hasNormals)
    {
        // Sphere map: coordinates from the vertex normal through GX_TEXMTX2, which the draw
        // loads per object (LoadEnvTexMtx) once its normal matrix is known.
        GX_SetTexCoordGen(GX_TEXCOORD0 + texIdx, GX_TG_MTX3x4, GX_TG_NRM, GX_TEXMTX2);
        gGxContext.mEnvMapBound = true;
    }
    else if (uvMap == UV_MAP_ENVIRONMENT)
    {
        // No normals in this draw's vertices (particles): use UV 0 rather than read a
        // normal that is not there.
        GX_SetTexCoordGen(GX_TEXCOORD0 + texIdx, GX_TG_MTX3x4, GX_TG_TEX0, GX_TEXMTX0);
    }
    else
    {
        GX_SetTexCoordGen(GX_TEXCOORD0 + texIdx, GX_TG_MTX3x4, GX_TG_TEX0 + uvMap, GX_TEXMTX0 + uvMap * 3);
    }
}

// Loads the indirect matrix that turns a warp map's bytes into offsets for one warped texture.
// The indirect unit reads the warp map's A, B and G as S, T and U, biased by -128
// (GX_ITB_STU), and adds M * [S T U] * 2^scaleExp to the stage's coordinates, in texels of the
// texture that stage samples. We want (byte - 128) / 256 * strength in normalised coordinates,
// that is (byte - 128) * strength * size / 256 texels: U (green) drives the S offset and T
// (blue) the T offset. The matrix holds values below 1 with 10 fractional bits, so the factor is
// split into a mantissa in [0.5, 1) and a power of two.
static void LoadWarpIndMtx(uint8_t indMtx, float strength, uint16_t width, uint16_t height)
{
    const float s = strength * float(width) / 256.0f;
    const float t = strength * float(height) / 256.0f;

    int32_t exp = 0;
    frexpf(std::max(s, t), &exp);
    exp = glm::clamp(exp, -17, 46);
    const float k = ldexpf(1.0f, -exp);

    f32 mtx[2][3] =
    {
        { 0.0f, 0.0f, std::min(s * k, 1023.0f / 1024.0f) },
        { 0.0f, std::min(t * k, 1023.0f / 1024.0f), 0.0f },
    };
    GX_SetIndTexMatrix(indMtx, mtx, int8_t(exp));
}

void GxResetIndirect()
{
    uint32_t mask = gGxContext.mIndirectTevMask;
    if (mask == 0)
    {
        return;
    }

    for (uint32_t stage = 0; stage < 16; ++stage)
    {
        if (mask & (1u << stage))
        {
            GX_SetTevDirect(stage);
        }
    }

    GX_SetNumIndStages(0);
    gGxContext.mIndirectTevMask = 0;
}

void BindMaterial(MaterialLite* material, bool useVertexColor, bool useBakedLighting, bool hasNormals)
{
    // If we are using vertex color modulation, then we need to use the first channel
    // since there is only one color vertex attribute.
    uint8_t matColorChannel = useVertexColor ? GX_COLOR1A1 : GX_COLOR0A0;

    ShadingModel shadingModel = material->GetShadingModel();
    BlendMode blendMode = material->GetBlendMode();
    VertexColorMode vertexColorMode = material->GetVertexColorMode();
    glm::vec4 color = material->GetColor();
    float opacity = material->GetOpacity();
    bool depthless = material->IsDepthTestDisabled();

    // Setup TexCoord matrices
    glm::vec2 uvOffset0 = material->GetUvOffset(0);
    glm::vec2 uvScale0 = material->GetUvScale(0);
    Mtx texMatrix0;
    guMtxTrans(texMatrix0, uvOffset0.x, uvOffset0.y, 0.0f);
    guMtxScaleApply(texMatrix0, texMatrix0, uvScale0.x, uvScale0.y, 1.0f);
    GX_LoadTexMtxImm(texMatrix0, GX_TEXMTX0, GX_TG_MTX3x4);

    glm::vec2 uvOffset1 = material->GetUvOffset(1);
    glm::vec2 uvScale1 = material->GetUvScale(1);
    Mtx texMatrix1;
    guMtxTrans(texMatrix1, uvOffset1.x, uvOffset1.y, 0.0f);
    guMtxScaleApply(texMatrix1, texMatrix1, uvScale1.x, uvScale1.y, 1.0f);
    GX_LoadTexMtxImm(texMatrix1, GX_TEXMTX1, GX_TG_MTX3x4);

    uint32_t tevStage = 0;
    bool vertexColorBlend = (vertexColorMode == VertexColorMode::TextureBlend);

    // A warp slot (TevMode::Warp) is an indirect map: it gets no colour stage of its own, and is
    // bound after the direct slots, so they keep the texcoords/texmaps they would have if it were
    // Pass. Indirect state left by the previous material is turned off first.
    GxResetIndirect();

    int32_t warpSlot = material->GetWarpSlot();
    Texture* warpTexture = (warpSlot >= 0) ? material->GetTexture(uint32_t(warpSlot)) : nullptr;

    // The TEV stage that samples each direct slot, and that slot's texture (for its size).
    uint8_t warpedStages[MATERIAL_LITE_MAX_TEXTURES] = {};
    GXTexObj* warpedTexObjs[MATERIAL_LITE_MAX_TEXTURES] = {};
    uint32_t numWarped = 0;

    uint32_t texIdx = 0;
    gGxContext.mEnvMapBound = false;
    for (uint32_t i = 0; i < 4; ++i)
    {
        Texture* texture = material->GetTexture(i);
        TevMode tevMode = (i == 0) ? TevMode::Replace : material->GetTevMode(i);
        uint32_t uvMap = material->GetUvMap(i);

        if (i == 0 && texture == nullptr)
            texture = Renderer::Get()->mWhiteTexture.Get<Texture>();

        if (tevMode != TevMode::Pass &&
            tevMode != TevMode::Warp &&
            tevMode != TevMode::Count &&
            texture != nullptr)
        {
            SetupSlotTexGen(texIdx, uvMap, hasNormals);
            GX_LoadTexObj(&texture->GetResource()->mGxTexObj, GX_TEXMAP0 + texIdx);

            tevStage = ConfigTev(tevStage, texIdx, tevMode, vertexColorBlend);

            // ConfigTev's last stage is the one that samples the texture.
            warpedStages[numWarped] = tevStage - 1;
            warpedTexObjs[numWarped] = &texture->GetResource()->mGxTexObj;
            numWarped++;

            texIdx++;
        }
    }

    if (warpTexture != nullptr && numWarped > 0)
    {
        // The warp map gets its own texcoord (scaled by its own size), generated like any slot's.
        SetupSlotTexGen(texIdx, material->GetUvMap(uint32_t(warpSlot)), hasNormals);
        GX_LoadTexObj(&warpTexture->GetResource()->mGxTexObj, GX_TEXMAP0 + texIdx);

        GX_SetNumIndStages(1);
        GX_SetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD0 + texIdx, GX_TEXMAP0 + texIdx);
        GX_SetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1);

        // One indirect matrix per direct slot (there are three, and at most three direct slots),
        // as the offset is in texels of the texture being warped.
        const float strength = material->GetWarpStrength();
        OCT_ASSERT(numWarped <= 3);
        for (uint32_t k = 0; k < numWarped; ++k)
        {
            LoadWarpIndMtx(GX_ITM_0 + k, strength,
                GX_GetTexObjWidth(warpedTexObjs[k]), GX_GetTexObjHeight(warpedTexObjs[k]));

            // As GX_SetTevIndWarp(stage, 0, GX_TRUE, GX_FALSE, mtx): signed 8-bit offsets added to
            // the stage's coordinates. Bias on all of S, T, U; only T (blue) and U (green) are used.
            GX_SetTevIndirect(warpedStages[k], GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_STU, GX_ITM_0 + k,
                GX_ITW_OFF, GX_ITW_OFF, GX_FALSE, GX_FALSE, GX_ITBA_OFF);
            gGxContext.mIndirectTevMask |= (1u << warpedStages[k]);
        }

        texIdx++;
    }

    GX_SetNumTexGens(texIdx);

    bool unlit = (shadingModel == ShadingModel::Unlit);
    gGxContext.mLighting.mEnabled = !unlit;

    bool applyColorScale = gGxContext.mColorScale != 1.0f && !(unlit && (!useVertexColor));

    // If we are using two reduced range values (vertex color + light color) then we need to scale twice.
    bool doubleColorScale = applyColorScale && !unlit && useVertexColor && !useBakedLighting;

    if (useVertexColor)
    {
        if (useBakedLighting)
        {
            // Compute the baked color first and save it out to a scratch register.
            // After resolving the final (dynamically lit) color, add this baked color to it.
            bool fullBake = (vertexColorMode != VertexColorMode::TextureBlend);

            GX_SetTevOrder(tevStage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
            GX_SetTevColorIn(tevStage, GX_CC_ZERO, fullBake ? GX_CC_RASC : GX_CC_RASA, GX_CC_CPREV, GX_CC_ZERO);
            GX_SetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
            GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
            GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
            tevStage++;
        }
        else
        {
            // Move vertex color to scratch register
            GX_SetTevOrder(tevStage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
            GX_SetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
            GX_SetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
            GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
            GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
            tevStage++;
        }
    }

    glm::vec4 materialColor = color;
    // A colour alpha over 1 (up to 2) doubles the alpha the material makes, as the PC renderer's
    // mColor does: the channel holds half of it and the TEV stage below scales its alpha by 2.
    const bool alphaX2 = (color.a > 1.0f);
    if (alphaX2)
        materialColor.a *= 0.5f;
    materialColor = glm::clamp(materialColor, 0.0f, 1.0f);
    float opacityScale = !(useVertexColor || unlit) ? gGxContext.mInvColorScale : 1.0f;

    // The material colour's alpha belongs here alongside the opacity slider. Forward.frag builds
    // its alpha as inColor.a * diffuse.a * mOpacity, and diffuse has already been multiplied by
    // mColor -- so the colour's alpha scales the result there. GX used to write the opacity alone
    // and drop materialColor.a on the floor, which made a Translucent material whose translucency
    // came from its colour render fully solid on console while looking correct in the editor. With
    // no texture bound, slot 0 falls back to the white texture, so the colour was the only alpha
    // the material had and the opacity slider was the only thing that still did anything.
    //
    // opacityScale is at most 1 (mColorScale is 1, 2 or 4), and alpha is at most 1, so the product
    // stays in range for the byte.
    GX_SetChanMatColor(matColorChannel, { uint8_t(materialColor.r * 255.0f),
                                        uint8_t(materialColor.g * 255.0f),
                                        uint8_t(materialColor.b * 255.0f),
                                        uint8_t(materialColor.a * opacity * 255.f * opacityScale) });

    glm::vec4 ambientColor = useBakedLighting ? glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) : gGxContext.mWorld->GetAmbientLightColor();
    ambientColor = glm::clamp(ambientColor * gGxContext.mInvColorScale, 0.0f, 1.0f);
    ambientColor.a = 1.0f;
    GX_SetChanAmbColor(matColorChannel, { uint8_t(ambientColor.r * 255.0f),
                                      uint8_t(ambientColor.g * 255.0f),
                                      uint8_t(ambientColor.b * 255.0f),
                                      uint8_t(ambientColor.a * 255.0f) });

    GX_SetTevOrder(tevStage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, matColorChannel);
    GX_SetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_CPREV, (unlit && useBakedLighting) ? GX_CC_ZERO : GX_CC_RASC, GX_CC_ZERO);
    GX_SetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_APREV, GX_CA_RASA, GX_CA_ZERO);
    GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, doubleColorScale ? gGxContext.mColorScaleEnum : GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, alphaX2 ? GX_CS_SCALE_2 : (doubleColorScale ? gGxContext.mColorScaleEnum : GX_CS_SCALE_1), GX_TRUE, GX_TEVPREV);
    tevStage++;

    // Specular, as the PC renderer's: the sun's highlight, its size the material's shininess, tinted by
    // its colour and scaled by its specular -- a second lighting channel (COLOR1) on a specular light
    // object, added. Not with vertex colours (they hold COLOR0, the lighting COLOR1) or without normals.
    gGxContext.mLighting.mSpecularMask = 0;
    gGxContext.mSpecTexBound = false;
    const float specular = material->GetSpecular();
    GXTexObj* lobeTex = (specular > 0.0f && material->IsSharpSpecular() && !unlit && hasNormals &&
                         gGxContext.mHasSpecLight && texIdx < 8) ? GetSpecLobeTex(material->GetShininess()) : nullptr;
    if (lobeTex != nullptr)
    {
        // per pixel: the lobe texture, its coordinates from the normal (GX_TEXMTX5, LoadEnvTexMtx), times
        // specular x the material's colour x the light's, added
        GX_SetTexCoordGen(GX_TEXCOORD0 + texIdx, GX_TG_MTX3x4, GX_TG_NRM, GX_TEXMTX5);
        GX_LoadTexObj(lobeTex, GX_TEXMAP0 + texIdx);
        GX_SetNumTexGens(texIdx + 1);

        const GXColor& lc = gGxContext.mSpecColor;
        glm::vec3 k = glm::clamp(glm::vec3(color) * specular * glm::vec3(lc.r, lc.g, lc.b) / 255.0f, 0.0f, 1.0f);
        GX_SetTevKColor(GX_KCOLOR3, { uint8_t(k.r * 255.0f), uint8_t(k.g * 255.0f), uint8_t(k.b * 255.0f), 255 });
        GX_SetTevKColorSel(tevStage, GX_TEV_KCSEL_K3);
        GX_SetTevOrder(tevStage, GX_TEXCOORD0 + texIdx, GX_TEXMAP0 + texIdx, GX_COLORNULL);
        GX_SetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_CPREV);
        GX_SetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        tevStage++;
        gGxContext.mSpecTexBound = true;
    }
    else if (specular > 0.0f && !unlit && !useVertexColor && hasNormals && gGxContext.mHasSpecLight &&
        gGxContext.mSceneNumLights < MAX_LIGHTS_PER_DRAW)
    {
        const uint8_t specLight = uint8_t(GX_LIGHT0 << gGxContext.mSceneNumLights);   // (the slot after the scene's)
        GXLightObj specObj;
        GX_InitSpecularDir(&specObj, gGxContext.mSpecDirVS.x, gGxContext.mSpecDirVS.y, gGxContext.mSpecDirVS.z);
        GX_InitLightShininess(&specObj, glm::max(material->GetShininess(), 1.0f));
        GX_InitLightColor(&specObj, gGxContext.mSpecColor);
        GX_LoadLightObj(&specObj, specLight);

        glm::vec3 specColor = glm::clamp(glm::vec3(color) * specular * gGxContext.mInvColorScale, 0.0f, 1.0f);
        GX_SetChanMatColor(GX_COLOR1A1, { uint8_t(specColor.r * 255.0f), uint8_t(specColor.g * 255.0f),
                                          uint8_t(specColor.b * 255.0f), 255 });
        GX_SetChanAmbColor(GX_COLOR1A1, { 0, 0, 0, 0 });

        GX_SetTevOrder(tevStage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR1A1);
        GX_SetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_RASC, GX_CC_ONE, GX_CC_CPREV);
        GX_SetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        tevStage++;
        gGxContext.mLighting.mSpecularMask = specLight;
    }

    // Vertex color modulation
    if (useVertexColor)
    {
        if (useBakedLighting)
        {
            // Add in the previously computed baked color to the dynamically lit result.
            GX_SetTevOrder(tevStage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
            GX_SetTevColorIn(tevStage, GX_CC_CPREV, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
            GX_SetTevAlphaIn(tevStage, GX_CA_APREV, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
            GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            tevStage++;
        }
        else if (vertexColorMode == VertexColorMode::Modulate)
        {
            GX_SetTevOrder(tevStage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
            GX_SetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_C0, GX_CC_CPREV, GX_CC_ZERO);
            GX_SetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_A0, GX_CA_APREV, GX_CA_ZERO);
            GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            tevStage++;
        }
        else if (vertexColorMode == VertexColorMode::TextureBlend)
        {
            GX_SetTevOrder(tevStage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
            GX_SetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_A0, GX_CC_CPREV, GX_CC_ZERO);
            GX_SetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_A0, GX_CA_APREV, GX_CA_ZERO);
            GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            tevStage++;
        }

        gGxContext.mLighting.mColorChannel = true;
    }
    else
    {
        gGxContext.mLighting.mColorChannel = false;
    }

    if (applyColorScale)
    {
        // Apply color scale
        GX_SetTevOrder(tevStage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
        GX_SetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, gGxContext.mColorScaleEnum, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, gGxContext.mColorScaleEnum, GX_TRUE, GX_TEVPREV);
        tevStage++;
    }

    OCT_ASSERT(tevStage <= 16);
    GX_SetNumTevStages(tevStage);

    gGxContext.mLighting.mMaterialSrc = GX_SRC_REG;

    // We need the alpha channel for doing simple shadows.
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);

    uint8_t depthEnabled = depthless ? GX_FALSE : GX_TRUE;

    // Blending
    if (blendMode == BlendMode::Opaque)
    {
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GX_SetZMode(depthEnabled, GX_LEQUAL, depthEnabled);
        GX_SetZCompLoc(GX_TRUE);
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    }
    else if (blendMode == BlendMode::Masked)
    {
        GX_SetAlphaCompare(GX_GREATER, int32_t(material->GetMaskCutoff() * 255.0f), GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZMode(depthEnabled, GX_LEQUAL, depthEnabled);
        GX_SetZCompLoc(GX_FALSE);
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    }
    else if (blendMode == BlendMode::Translucent)
    {
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GX_SetZMode(depthEnabled, GX_LEQUAL, GX_FALSE);
        GX_SetZCompLoc(GX_TRUE);
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    }
    else if (blendMode == BlendMode::Additive)
    {
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GX_SetZMode(depthEnabled, GX_LEQUAL, GX_FALSE);
        GX_SetZCompLoc(GX_TRUE);
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
    }
    else if (blendMode == BlendMode::Subtractive)
    {
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GX_SetZMode(depthEnabled, GX_LEQUAL, GX_FALSE);
        GX_SetZCompLoc(GX_TRUE);
        GX_SetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
    }

    // the alpha band: kept only at most the one value or at least the other (GX_LEQUAL OR GX_GEQUAL)
    if (material->IsLite() && ((MaterialLite*)material)->HasAlphaBand())
    {
        MaterialLite* lite = (MaterialLite*)material;
        GX_SetAlphaCompare(GX_LEQUAL, uint8_t(glm::clamp(lite->GetAlphaKeepBelow(), 0.0f, 1.0f) * 255.0f), GX_AOP_OR,
                           GX_GEQUAL, uint8_t(glm::clamp(lite->GetAlphaKeepAbove(), 0.0f, 1.0f) * 255.0f));
        GX_SetZCompLoc(GX_FALSE);
    }

    bool matApplyFog = material->ShouldApplyFog();
    if (gGxContext.mFogType != GX_FOG_NONE && 
        gGxContext.mApplyFog != matApplyFog)
    {
        gGxContext.mApplyFog = matApplyFog;

        // Need to enable/disable fog because of material property
        if (material->ShouldApplyFog())
        {
            GX_SetFog(
                gGxContext.mFogType,
                gGxContext.mFogStartZ,
                gGxContext.mFogEndZ,
                gGxContext.mFogNearZ,
                gGxContext.mFogFarZ,
                gGxContext.mFogColor);
        }
        else
        {
            GX_SetFog(
                GX_FOG_NONE,
                0.0f,
                0.0f,
                0.0f,
                0.0f,
                {0,0,0,0});
        }
    }

    CullMode cullMode = material->GetCullMode();
    switch (cullMode)
    {
        // Note: Culling is reversed on GX for some reason.
        case CullMode::None: GX_SetCullMode(GX_CULL_NONE); break;
        case CullMode::Back: GX_SetCullMode(GX_CULL_FRONT); break;
        case CullMode::Front: GX_SetCullMode(GX_CULL_BACK); break;

        default: break;
    }
}

// COMPACT UNLIT MESHES (opt in: GFX_SetCompactUnlitMeshes, from Lua Renderer.SetCompactUnlitMeshes).
// A mesh with vertex colours and an unlit, untextured material draws with its colours and
// nothing else, yet it was kept as 44 bytes a vertex (normal, two sets of texture coordinates)
// and 10 bytes a triangle corner in its display list. Compact, it is 16 and 4: a GameCube stage's
// pipe went from 4.2 MB to 1.5 MB. Opt in because a compact mesh cannot then be drawn with a lit
// or textured material put on it by a node -- there are no normals or coordinates left to draw it
// with; it draws as its colours.
static bool sCompactUnlitMeshes = false;

void GFX_SetCompactUnlitMeshes(bool compact)
{
    sCompactUnlitMeshes = compact;
}

bool GFX_GetCompactUnlitMeshes()
{
    return sCompactUnlitMeshes;
}

// QUANTIZED MESHES (opt in: GFX_SetQuantizedMeshes, from Lua Renderer.SetQuantizedMeshes). A lit,
// textured mesh with no vertex colours and no triangle collision is kept in the GameCube's own
// compressed vertex formats, as its games kept their models: positions as 16-bit fixed point (the
// fraction bits chosen per mesh from its extent: a model a few units across gets 1/4096 of a unit or
// finer), normals as 8-bit (6 fraction bits, the hardware's), texture coordinates as 16-bit. The GPU
// expands them as it reads them, so lighting, environment mapping and the materials' UV transforms
// see the same values. 14 bytes a vertex for 40, and the float array is let go: a 912-vertex
// character frame went from 36 KB of vertices to 13. Meshes whose second UV set differs from the
// first keep the floats (the second set is not kept), as do meshes too big to fit 16 bits finely.
static bool sQuantizedMeshes = false;

void GFX_SetQuantizedMeshes(bool quantized)
{
    sQuantizedMeshes = quantized;
}

bool GFX_GetQuantizedMeshes()
{
    return sQuantizedMeshes;
}

// EXTENDED: the meshes the above leaves as floats -- vertex colours (lit or not: the colours kept as
// they are, 4 bytes), a second UV set of its own (kept as a second 16-bit set, its own fraction bits)
// -- quantized too: a mesh with both, 44 bytes a vertex as floats, is 22. Its own switch so a game
// that turned on GFX_SetQuantizedMeshes before this existed draws as it did.
static bool sQuantizedMeshesExtended = false;
static GxQuantizeFilter sQuantizeFilter = nullptr;

void GFX_SetQuantizedMeshesExtended(bool extended)
{
    sQuantizedMeshesExtended = extended;
}

bool GFX_GetQuantizedMeshesExtended()
{
    return sQuantizedMeshesExtended;
}

void GFX_SetQuantizeFilter(GxQuantizeFilter filter)
{
    sQuantizeFilter = filter;
}

bool GFX_QuantizeAllowed(StaticMesh* mesh)
{
    return sQuantizeFilter == nullptr || sQuantizeFilter(mesh);
}

// ANISOTROPIC FILTERING: a mipmapped texture seen at a slant is blurred to the mip its most squeezed direction asks for;
// on a mesh of flat triangles each tilted its own way (a swell grid) that mip jumps at their edges -- a darker, blurrier
// band with a straight edge. Anisotropy takes the mip from the less squeezed direction and filters along the other
// (extra filter cycles where it's needed, none where it isn't), so the slant stays sharp and the jumps go. Opt in,
// texture by texture: the games before it draw as they did.
static GxAnisotropyFilter sAnisotropyFilter = nullptr;

void GFX_SetTextureAnisotropy(GxAnisotropyFilter filter)
{
    sAnisotropyFilter = filter;
}

uint8_t GFX_TextureAnisotropy(Texture* texture)
{
    return sAnisotropyFilter != nullptr ? sAnisotropyFilter(texture) : 1;
}

// TRILINEAR, or not: blending two mips costs the texture unit two cycles a pixel, one mip one. A shop interior, wood on
// most of the screen several layers deep, was fill-bound on its grain's trilinear read (2026-10-21: the wood's grain
// alone cost as much as its grain and AO map read bilinearly). Opt out, texture by texture.
static GxTrilinearFilter sTrilinearFilter = nullptr;

void GFX_SetTextureTrilinear(GxTrilinearFilter filter)
{
    sTrilinearFilter = filter;
}

bool GFX_TextureTrilinear(Texture* texture)
{
    return sTrilinearFilter != nullptr ? sTrilinearFilter(texture) : true;
}

// Unlit, untextured, and its vertex colours not blending textures: drawn by colour alone.
bool GFX_MaterialAllowsCompact(Material* material)
{
    MaterialLite* lite = Material::AsLite(material);
    if (lite == nullptr || lite->GetShadingModel() != ShadingModel::Unlit ||
        lite->GetVertexColorMode() == VertexColorMode::TextureBlend)
    {
        return false;
    }

    // An empty slot answers with the engine's white texture: white read at any coordinate is white,
    // so that draws the same with no coordinates at all.
    Texture* white = Renderer::Get()->mWhiteTexture.Get<Texture>();
    for (uint32_t slot = 0; slot < MATERIAL_LITE_MAX_TEXTURES; ++slot)
    {
        Texture* texture = lite->GetTexture(slot);
        if (texture != nullptr && texture != white)
        {
            return false;
        }
    }

    return true;
}

void BindStaticMesh(StaticMesh* staticMesh, uint32_t* instanceColors)
{
    StaticMeshResource* resource = staticMesh->GetResource();
    if (resource->mCompact)
    {
        // position and colour, from the compact array (instance colours cannot apply: the colours
        // are the mesh's own, and its source arrays are gone)
        GX_ClearVtxDesc();
        GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
        GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX16);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        uint8_t* compact = (uint8_t*)resource->mCompactVertices;
        GX_SetArray(GX_VA_POS, compact, 16);
        GX_SetArray(GX_VA_CLR0, compact + 12, 16);
        return;
    }

    if (resource->mQuantized)
    {
        // 16-bit position, 8-bit normal, a pad byte, 16-bit texture coordinate (14 bytes); then,
        // extended, the mesh's own second texture coordinate and its colour where it has them
        // (mQuantStride, mQuantUv1, mQuantClr) -- else both texture coordinate inputs read the one
        // set. Flushed and the vertex cache cleared when it was made (GFX_CreateStaticMeshResource):
        // nothing writes it after.
        uint8_t* q = (uint8_t*)resource->mQuantVertices;
        const uint32_t stride = resource->mQuantStride;
        const bool color = (instanceColors != nullptr || resource->mQuantClr != 0);
        GX_ClearVtxDesc();
        GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
        GX_SetVtxDesc(GX_VA_NRM, GX_INDEX16);
        if (color)
        {
            GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX16);
        }
        GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX16);
        GX_SetVtxDesc(GX_VA_TEX1, GX_INDEX16);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, resource->mPosFrac);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_S8, 6);
        if (color)
        {
            GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        }
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_S16, resource->mUvFrac);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_S16,
                         resource->mQuantUv1 != 0 ? resource->mUv1Frac : resource->mUvFrac);
        GX_SetArray(GX_VA_POS, q, stride);
        GX_SetArray(GX_VA_NRM, q + 6, stride);
        if (instanceColors != nullptr)
        {
            GX_SetArray(GX_VA_CLR0, instanceColors, sizeof(uint32_t));
            DCFlushRange(instanceColors, staticMesh->GetNumVertices() * sizeof(uint32_t));
            GX_InvVtxCache();
        }
        else if (resource->mQuantClr != 0)
        {
            GX_SetArray(GX_VA_CLR0, q + resource->mQuantClr, stride);
        }
        GX_SetArray(GX_VA_TEX0, q + 10, stride);
        GX_SetArray(GX_VA_TEX1, q + (resource->mQuantUv1 != 0 ? resource->mQuantUv1 : 10), stride);
        return;
    }

    uint8_t* vertBytes = staticMesh->HasVertexColor() ? (uint8_t*)staticMesh->GetColorVertices() : (uint8_t*)staticMesh->GetVertices();
    uint32_t numVertices = staticMesh->GetNumVertices();

    uint32_t posOffset = offsetof(Vertex, mPosition);
    uint32_t nrmOffset = offsetof(Vertex, mNormal);
    uint32_t clrOffset = 0;
    uint32_t texOffset0 = offsetof(Vertex, mTexcoord0);
    uint32_t texOffset1 = offsetof(Vertex, mTexcoord1);

    if (staticMesh->HasVertexColor())
    {
        posOffset = offsetof(VertexColor, mPosition);
        nrmOffset = offsetof(VertexColor, mNormal);
        clrOffset = offsetof(VertexColor, mColor);
        texOffset0 = offsetof(VertexColor, mTexcoord0);
        texOffset1 = offsetof(VertexColor, mTexcoord1);
    }

    bool hasColor = (staticMesh->HasVertexColor() || instanceColors != nullptr);

    // Set Vertex Format
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_NRM, GX_INDEX16);
    if (hasColor)
    {
        GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX16);
    }
    GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_TEX1, GX_INDEX16);

    // Set Attribute Formats
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    if (hasColor)
    {
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    }
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);

    // Set Array
    uint32_t vertexSize = staticMesh->GetVertexSize();
    GX_SetArray(GX_VA_POS, vertBytes + posOffset, vertexSize);
    GX_SetArray(GX_VA_NRM, vertBytes + nrmOffset, vertexSize);
    if (hasColor)
    {
        if (instanceColors != nullptr)
        {
            GX_SetArray(GX_VA_CLR0, instanceColors, sizeof(uint32_t));
        }
        else
        {
            GX_SetArray(GX_VA_CLR0, vertBytes + clrOffset, vertexSize);
        }
    }
    GX_SetArray(GX_VA_TEX0, vertBytes + texOffset0, vertexSize);
    GX_SetArray(GX_VA_TEX1, vertBytes + texOffset1, vertexSize);

    // TODO: Are both of these cache functions necessary to call?
    DCFlushRange(vertBytes, numVertices * vertexSize);

    if (instanceColors != nullptr)
    {
        DCFlushRange(instanceColors, numVertices * sizeof(uint32_t));
    }

    GX_InvVtxCache();
}

#if OCT_SKELETAL
void BindSkeletalMesh(SkeletalMesh* skeletalMesh)
{
    uint8_t* vertBytes = (uint8_t*)skeletalMesh->GetVertices().data();
    uint32_t numVertices = skeletalMesh->GetNumVertices();

    //uint32_t mtxOffset = offsetof(VertexSkinned, mBoneIndices);
    uint32_t posOffset = offsetof(VertexSkinned, mPosition);
    uint32_t nrmOffset = offsetof(VertexSkinned, mNormal);
    uint32_t texOffset0 = offsetof(VertexSkinned, mTexcoord0);
    uint32_t texOffset1 = offsetof(VertexSkinned, mTexcoord1);

    // Set Vertex Format
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_PTNMTXIDX, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_NRM, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_TEX1, GX_INDEX16);

    // Set Attribute Formats
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_PTNMTXIDX, 0, GX_U8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);

    // Set Array
    uint32_t vertexSize = sizeof(VertexSkinned);
    //GX_SetArray(GX_VA_PTNMTXIDX, vertBytes + mtxOffset, vertexSize);
    GX_SetArray(GX_VA_POS, vertBytes + posOffset, vertexSize);
    GX_SetArray(GX_VA_NRM, vertBytes + nrmOffset, vertexSize);
    GX_SetArray(GX_VA_TEX0, vertBytes + texOffset0, vertexSize);
    GX_SetArray(GX_VA_TEX1, vertBytes + texOffset1, vertexSize);

    // TODO: Are both of these cache functions necessary to call?
    DCFlushRange(vertBytes, numVertices * vertexSize);
    GX_InvVtxCache();
}
#endif

uint8_t ConfigTev(uint8_t tevStage, uint32_t textureSlot, TevMode mode, bool vertexColorBlend)
{
    uint8_t blendChannel = GX_CH_RED;

    switch (textureSlot)
    {
        case 0: blendChannel = GX_CH_RED; break;
        case 1: blendChannel = GX_CH_GREEN; break;
        case 2: blendChannel = GX_CH_BLUE; break;
    }

    if (textureSlot == 0)
    {
        mode = TevMode::Replace;
    }

    if (vertexColorBlend)
    {
        if (gGxContext.mColorScale != 1.0f)
        {
            GX_SetTevSwapMode(tevStage, GX_TEV_SWAP1 + textureSlot, GX_TEV_SWAP0);
            GX_SetTevSwapModeTable(GX_TEV_SWAP1 + textureSlot, blendChannel, blendChannel, blendChannel, GX_CH_ALPHA);

            // Multiply the RGB vertex colors based on color scale. Leave alpha unchanged
            GX_SetTevOrder(tevStage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
            GX_SetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_RASC, GX_CC_ONE, GX_CC_ZERO);
            GX_SetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
            GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, gGxContext.mColorScaleEnum, GX_TRUE, GX_TEVREG0);
            GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, gGxContext.mColorScaleEnum, GX_TRUE, GX_TEVREG0);
            tevStage++;

            uint8_t prevColor = textureSlot == 0 ? GX_CC_ZERO : GX_CC_CPREV;

            // Then do the texture blending
            GX_SetTevOrder(tevStage, GX_TEXCOORD0 + textureSlot, GX_TEXMAP0 + textureSlot, GX_COLOR0A0);
            GX_SetTevColorIn(tevStage, prevColor, GX_CC_TEXC, GX_CC_C0, GX_CC_ZERO);
            GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GX_SetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
            GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        }
        else
        {
            GX_SetTevSwapMode(tevStage, GX_TEV_SWAP1 + textureSlot, GX_TEV_SWAP0);
            GX_SetTevSwapModeTable(GX_TEV_SWAP1 + textureSlot, blendChannel, blendChannel, blendChannel, GX_CH_ALPHA);

            uint8_t prevColor = textureSlot == 0 ? GX_CC_ZERO : GX_CC_CPREV;

            GX_SetTevOrder(tevStage, GX_TEXCOORD0 + textureSlot, GX_TEXMAP0 + textureSlot, GX_COLOR0A0);
            GX_SetTevColorIn(tevStage, prevColor, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
            GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GX_SetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
            GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        }
    }
    else
    {
        GX_SetTevSwapMode(tevStage, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GX_SetTevOrder(tevStage, GX_TEXCOORD0 + textureSlot, GX_TEXMAP0 + textureSlot, GX_COLOR0A0);

        switch (mode)
        {
            case TevMode::Modulate:
                GX_SetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO);
                GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
                GX_SetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_TEXA, GX_CA_APREV, GX_CA_ZERO);
                GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            break;

            case TevMode::Decal:
                GX_SetTevColorIn(tevStage, GX_CC_CPREV, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO);
                GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
                GX_SetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
                GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            break;

            case TevMode::Add:
                GX_SetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_CPREV, GX_CC_ONE, GX_CC_TEXC);
                GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
                GX_SetTevAlphaIn(tevStage, GX_CA_TEXA, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
                GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            break;

            case TevMode::SignedAdd:
                GX_SetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_CPREV);
                GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_SUBHALF, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
                GX_SetTevAlphaIn(tevStage, GX_CA_TEXA, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
                GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_SUBHALF, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            break;
            
            case TevMode::Subtract:
                GX_SetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_CPREV);
                GX_SetTevColorOp(tevStage, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
                GX_SetTevAlphaIn(tevStage, GX_CA_TEXA, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
                GX_SetTevAlphaOp(tevStage, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            break;

            case TevMode::Contour:
                // prev + (prev.r == tex.a ? tex.rgb : 0); alpha passes through
                GX_SetTevColorIn(tevStage, GX_CC_CPREV, GX_CC_TEXA, GX_CC_TEXC, GX_CC_CPREV);
                GX_SetTevColorOp(tevStage, GX_TEV_COMP_R8_EQ, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
                GX_SetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
                GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            break;

            case TevMode::Replace:
            default:
                GX_SetTevColorIn(tevStage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
                GX_SetTevColorOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
                GX_SetTevAlphaIn(tevStage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
                GX_SetTevAlphaOp(tevStage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            break;
        }
    }

    ++tevStage;
    return tevStage;
}

void ApplyWidgetRotation(Mtx& mtx, Widget* widget)
{
    // Ok the position / rotation / scaling of widgets is in a weird place now.
    // I think everything could be calculated and stored in the widget transform.
    // I think the reason this wasn't the case was because I wanted to batch quad draws together
    // so I needed to have all of there vertices precomputed. But I'm not sure if that will ever be 
    // a possibility, so perhaps just fix the widget transform so that it stores the entire true transform
    // and just let Quads use a static vertex buffer.

    Mtx rotMat;
    guMtxIdentity(rotMat);

    const glm::mat3& trans3 = widget->GetTransform();
    rotMat[0][0] = trans3[0][0];
    rotMat[0][1] = trans3[1][0];
    rotMat[1][0] = trans3[0][1];
    rotMat[1][1] = trans3[1][1];
    rotMat[0][3] = trans3[2][0];
    rotMat[1][3] = trans3[2][1];

    Mtx srcMat;
    memcpy(srcMat, &mtx, sizeof(float) * 4 * 3);

    c_guMtxConcat(rotMat, srcMat, mtx);
}

// A MESH'S DISPLAY LISTS: its faces in batches of at most kBatchFaces, each its own list in its own
// block, with its own bounding sphere, and each sent as triangle strips.
//
// Small blocks: it was one list for the whole mesh, 636 KB in one piece for a pipe piece of 21,000
// triangles, which a GameCube that has been streaming stages in and out for a while no longer has
// anywhere (see BigBlockCache_Dolphin.cpp).
//
// Bounding spheres: a batch is a run of the mesh's faces as they were made, which for a mesh built
// along a path (a pipe, a track) is a stretch of it. CallMeshDisplayListCulled leaves out the
// batches off screen, so a big piece half in view sends half its triangles. NOT HOOKED UP: see
// GxUtils.h for how. (The spheres are still worked out as the lists are built; it costs little.)
//
// Strips: a list of separate triangles makes the GPU transform three vertices for every triangle,
// shared or not -- it keeps no transformed vertices to reuse. In a strip each triangle after the
// first adds ONE vertex. Measured on hardware (2026-09-24), the frame's GPU time
// went with the triangles sent, about 0.25 ms a thousand, with the transform unit waiting on
// triangle setup nearly all the time it was busy. The triangles and their winding are exactly the
// mesh's: only the order they are sent in changes.
//
// The handle the renderer keeps (mDisplayList, mColorDisplayList) is a MeshLists; only these
// functions look inside it.
namespace
{
    const uint32_t kBatchFaces = 1024;

    struct MeshListPart
    {
        void* mList;
        uint32_t mSize;
        float mCenter[3];       // bounding sphere, in the mesh's own space
        float mRadius;
        uint16_t mVerts;        // vertices and triangles it sends, for the perf log
        uint16_t mTris;
    };

    struct MeshLists
    {
        uint32_t mCount;
        MeshListPart mParts[1];             // mCount of them
    };

    // The GPU may still be reading lists (GX_CallDispList only queues the call): see GxDeferFree.
    void FreeMeshLists(MeshLists* lists, uint32_t built)
    {
        for (uint32_t i = 0; i < built; ++i)
        {
            GxDeferFree(lists->mParts[i].mList);
        }
        free(lists);
    }

    // ---- the stripper. Scratch for one batch, kept (static) rather than allocated per mesh, so
    // building lists makes no temporary holes in the heap.
    struct EdgeEntry
    {
        uint32_t mKey;          // the edge's two vertices, lower << 16 | higher
        uint16_t mFace;
    };

    EdgeEntry sEdges[kBatchFaces * 3];
    uint32_t sNumEdges = 0;
    uint16_t sFaceMark[kBatchFaces];       // 0 free; kUsed taken; anything else: this try's walk
    uint16_t sMarkNow = 0;
    const uint16_t kUsed = 0xffff;
    uint16_t sStrips[kBatchFaces * 3];     // the strips' vertices, one after another
    uint16_t sStripLen[kBatchFaces];
    uint16_t sSingles[kBatchFaces * 3];    // triangles no strip took, as a plain list
    uint16_t sWalk[kBatchFaces + 2];
    const IndexType* sFaceIdx = nullptr;   // the batch's indices, three a face

    inline uint16_t FaceV(uint32_t f, uint32_t k) { return uint16_t(sFaceIdx[f * 3 + k]); }
    inline uint32_t EdgeKey(uint16_t a, uint16_t b) { return (a < b) ? (uint32_t(a) << 16 | b) : (uint32_t(b) << 16 | a); }

    // A face not yet taken (nor walked this try) that has the directed edge u -> v; its third
    // vertex in `x`. -1 if there is none.
    int32_t NextFace(uint16_t u, uint16_t v, uint16_t mark, uint16_t& x)
    {
        const uint32_t key = EdgeKey(u, v);
        const EdgeEntry* e = std::lower_bound(sEdges, sEdges + sNumEdges, key,
                                              [](const EdgeEntry& a, uint32_t k) { return a.mKey < k; });
        for (; e < sEdges + sNumEdges && e->mKey == key; ++e)
        {
            const uint16_t f = e->mFace;
            if (sFaceMark[f] == kUsed || sFaceMark[f] == mark)
            {
                continue;
            }
            const uint16_t a = FaceV(f, 0), b = FaceV(f, 1), c = FaceV(f, 2);
            if (a == u && b == v) { x = c; return f; }
            if (b == u && c == v) { x = a; return f; }
            if (c == u && a == v) { x = b; return f; }
        }
        return -1;
    }

    // A strip from face f, starting at its vertex `rot`, as far as it goes; its length in vertices.
    // Triangle k of a strip is (s[k], s[k+1], s[k+2]) for even k and (s[k+1], s[k], s[k+2]) for odd
    // (how GX, like GL, keeps a strip's winding), and each must be its face's own winding -- so the
    // next face must hold the directed edge s[n-2] -> s[n-1] (even) or s[n-1] -> s[n-2] (odd).
    uint32_t Walk(uint32_t f, uint32_t rot, bool take, uint16_t* out)
    {
        sMarkNow = uint16_t(sMarkNow + 1);
        if (sMarkNow == 0 || sMarkNow == kUsed)
        {
            // wrapped: forget old tries (the taken faces stay taken)
            for (uint32_t i = 0; i < kBatchFaces; ++i)
            {
                if (sFaceMark[i] != kUsed) sFaceMark[i] = 0;
            }
            sMarkNow = 1;
        }
        const uint16_t mark = take ? kUsed : sMarkNow;

        out[0] = FaceV(f, rot);
        out[1] = FaceV(f, (rot + 1) % 3);
        out[2] = FaceV(f, (rot + 2) % 3);
        sFaceMark[f] = mark;
        uint32_t n = 3;
        for (uint32_t k = 1; ; ++k)
        {
            const bool even = (k & 1) == 0;
            const uint16_t u = even ? out[n - 2] : out[n - 1];
            const uint16_t v = even ? out[n - 1] : out[n - 2];
            uint16_t x = 0;
            const int32_t g = NextFace(u, v, sMarkNow, x);
            if (g < 0)
            {
                break;
            }
            sFaceMark[g] = mark;
            out[n++] = x;
        }
        return n;
    }

    struct Stripped
    {
        uint32_t mStrips;       // in sStripLen / sStrips
        uint32_t mStripVerts;
        uint32_t mSingles;      // vertices in sSingles
        uint32_t mTris;
    };

    Stripped StripBatch(const IndexType* faceIdx, uint32_t faces)
    {
        sFaceIdx = faceIdx;
        sNumEdges = 0;
        for (uint32_t f = 0; f < faces; ++f)
        {
            sFaceMark[f] = 0;
            const uint16_t a = FaceV(f, 0), b = FaceV(f, 1), c = FaceV(f, 2);
            if (a == b || b == c || c == a)
            {
                sFaceMark[f] = kUsed;       // no area, nothing drawn: left out
                continue;
            }
            sEdges[sNumEdges++] = { EdgeKey(a, b), uint16_t(f) };
            sEdges[sNumEdges++] = { EdgeKey(b, c), uint16_t(f) };
            sEdges[sNumEdges++] = { EdgeKey(c, a), uint16_t(f) };
        }
        std::sort(sEdges, sEdges + sNumEdges, [](const EdgeEntry& x, const EdgeEntry& y) { return x.mKey < y.mKey; });
        sMarkNow = 0;

        Stripped s = {};
        for (uint32_t f = 0; f < faces; ++f)
        {
            if (sFaceMark[f] == kUsed)
            {
                continue;
            }
            // whichever corner makes the longest strip
            uint32_t bestRot = 0, bestLen = 0;
            for (uint32_t rot = 0; rot < 3; ++rot)
            {
                const uint32_t len = Walk(f, rot, false, sWalk);
                if (len > bestLen)
                {
                    bestLen = len;
                    bestRot = rot;
                }
            }
            uint16_t* out = sStrips + s.mStripVerts;
            const uint32_t len = Walk(f, bestRot, true, out);
            s.mTris += len - 2;
            if (len == 3)
            {
                // one triangle: cheaper in the plain list than as a strip of its own
                sSingles[s.mSingles++] = out[0];
                sSingles[s.mSingles++] = out[1];
                sSingles[s.mSingles++] = out[2];
                continue;
            }
            sStripLen[s.mStrips++] = uint16_t(len);
            s.mStripVerts += len;
        }
        return s;
    }

    // ---- culling
    bool sCullEnabled = false;
    float sTanX = 0.0f, sTanY = 0.0f, sNearZ = 0.0f, sFarZ = 0.0f;
    float sInvLenX = 1.0f, sInvLenY = 1.0f;

    // Is a sphere (in the mesh's space) wholly outside the view? View space looks down -Z.
    bool SphereOffscreen(const MeshListPart& p, const Mtx mv, float scale)
    {
        const float cx = p.mCenter[0], cy = p.mCenter[1], cz = p.mCenter[2];
        const float x = mv[0][0] * cx + mv[0][1] * cy + mv[0][2] * cz + mv[0][3];
        const float y = mv[1][0] * cx + mv[1][1] * cy + mv[1][2] * cz + mv[1][3];
        const float z = mv[2][0] * cx + mv[2][1] * cy + mv[2][2] * cz + mv[2][3];
        const float r = p.mRadius * scale;

        if (z > -sNearZ + r) return true;                       // behind the near plane
        if (-z > sFarZ + r) return true;                        // past the far one
        if ((x + sTanX * z) * sInvLenX > r) return true;        // right
        if ((-x + sTanX * z) * sInvLenX > r) return true;       // left
        if ((y + sTanY * z) * sInvLenY > r) return true;        // top
        if ((-y + sTanY * z) * sInvLenY > r) return true;       // bottom
        return false;
    }
}

void GxSetCullFrustum(bool enabled, float tanHalfX, float tanHalfY, float nearZ, float farZ)
{
    sCullEnabled = enabled && tanHalfX > 0.0f && tanHalfY > 0.0f;
    sTanX = tanHalfX;
    sTanY = tanHalfY;
    sNearZ = nearZ;
    sFarZ = farZ;
    sInvLenX = 1.0f / sqrtf(1.0f + tanHalfX * tanHalfX);
    sInvLenY = 1.0f / sqrtf(1.0f + tanHalfY * tanHalfY);
}

// A mesh's display lists, if it has them: a mesh that ran out of memory building them has none.
void CallMeshDisplayList(void* displayList, uint32_t size)
{
    MeshLists* lists = (MeshLists*)displayList;
    if (lists == nullptr || size == 0)
    {
        return;
    }

    for (uint32_t i = 0; i < lists->mCount; ++i)
    {
        GX_CallDispList(lists->mParts[i].mList, lists->mParts[i].mSize);
        GxCountDraw(lists->mParts[i].mVerts, lists->mParts[i].mTris);
    }
}

void CallMeshDisplayListCulled(void* displayList, uint32_t size, const Mtx modelView)
{
    MeshLists* lists = (MeshLists*)displayList;
    if (lists == nullptr || size == 0)
    {
        return;
    }
    if (!sCullEnabled)
    {
        CallMeshDisplayList(displayList, size);
        return;
    }

    // how much the model-view matrix grows a length: its longest axis
    float scale2 = 0.0f;
    for (uint32_t c = 0; c < 3; ++c)
    {
        const float l2 = modelView[0][c] * modelView[0][c] + modelView[1][c] * modelView[1][c] + modelView[2][c] * modelView[2][c];
        scale2 = (l2 > scale2) ? l2 : scale2;
    }
    const float scale = sqrtf(scale2);

    for (uint32_t i = 0; i < lists->mCount; ++i)
    {
        const MeshListPart& part = lists->mParts[i];
        if (!SphereOffscreen(part, modelView, scale))
        {
            GX_CallDispList(part.mList, part.mSize);
            GxCountDraw(part.mVerts, part.mTris);
        }
    }
}

void* CreateMeshDisplayList(StaticMesh* staticMesh, bool useColor, uint32_t& outSize, bool compact)
{
    outSize = 0;
    IndexType* indices = staticMesh->GetIndices();
    const uint32_t numFaces = staticMesh->GetNumFaces();
    if (numFaces == 0 || indices == nullptr)
    {
        return nullptr;
    }

    // Where the positions are, for the bounds: first in every vertex format.
    const uint8_t* posBase = nullptr;
    uint32_t posStride = 0;
    if (staticMesh->HasCompactVertices())
    {
        posBase = (const uint8_t*)staticMesh->GetColorVertices();
        posStride = 16;                                     // x, y, z, colour
    }
    else if (staticMesh->HasVertexColor())
    {
        posBase = (const uint8_t*)staticMesh->GetColorVertices();
        posStride = sizeof(VertexColor);
    }
    else
    {
        posBase = (const uint8_t*)staticMesh->GetVertices();
        posStride = sizeof(Vertex);
    }

    // position, normal, colour, two texture coordinates (16-bit indices each); compact: position, colour
    const uint32_t elemSize = compact ? (2 + 2) : useColor ? (2 + 2 + 2 + 2 + 2) : (2 + 2 + 2 + 2);
    const uint32_t numLists = (numFaces + kBatchFaces - 1) / kBatchFaces;
    MeshLists* lists = (MeshLists*)malloc(sizeof(MeshLists) + (numLists - 1) * sizeof(MeshListPart));
    if (lists == nullptr)
    {
        LogError("Mesh %s: out of memory for its display lists", staticMesh->GetName().c_str());
        return nullptr;
    }
    lists->mCount = numLists;

    auto emit = [&](uint16_t index)
    {
        GX_Position1x16(index);
        if (compact)
        {
            GX_Color1x16(index);
            return;
        }
        GX_Normal1x16(index);
        if (useColor)
        {
            GX_Color1x16(index);
        }
        GX_TexCoord1x16(index);
        GX_TexCoord1x16(index);
    };

    uint32_t total = 0;

    for (uint32_t part = 0; part < numLists; ++part)
    {
        const uint32_t firstFace = part * kBatchFaces;
        const uint32_t faces = (numFaces - firstFace < kBatchFaces) ? (numFaces - firstFace) : kBatchFaces;
        const IndexType* faceIdx = indices + firstFace * 3;
        MeshListPart& out = lists->mParts[part];

        // the batch's bounding sphere: the middle of its box, out to the farthest corner
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        if (posBase != nullptr)
        {
            for (uint32_t i = 0; i < faces * 3; ++i)
            {
                const float* p = (const float*)(posBase + uint32_t(faceIdx[i]) * posStride);
                for (uint32_t k = 0; k < 3; ++k)
                {
                    lo[k] = (p[k] < lo[k]) ? p[k] : lo[k];
                    hi[k] = (p[k] > hi[k]) ? p[k] : hi[k];
                }
            }
            float r2 = 0.0f;
            for (uint32_t k = 0; k < 3; ++k)
            {
                out.mCenter[k] = 0.5f * (lo[k] + hi[k]);
                const float h = 0.5f * (hi[k] - lo[k]);
                r2 += h * h;
            }
            out.mRadius = sqrtf(r2);
        }
        else
        {
            out.mCenter[0] = out.mCenter[1] = out.mCenter[2] = 0.0f;
            out.mRadius = 1e30f;                            // no positions to go on: never left out
        }

        const Stripped s = StripBatch(faceIdx, faces);
        out.mVerts = uint16_t(s.mStripVerts + s.mSingles);
        out.mTris = uint16_t(s.mTris);

        // exactly what it holds: each primitive is a command byte and a 16-bit count, then its vertices
        uint32_t bytes = s.mStrips * 3 + s.mStripVerts * elemSize;
        if (s.mSingles > 0)
        {
            bytes += 3 + s.mSingles * elemSize;
        }
        uint32_t allocSize = (bytes + 0x1f) & (~0x1f);  // 32 byte aligned
        allocSize += 64;                                // extra space to account for the pipe flush
        void* displayList = memalign(32, allocSize);

        // Out of memory: no lists at all, and the mesh is not drawn (CallMeshDisplayList skips
        // it), rather than lists written through a null pointer.
        if (displayList == nullptr)
        {
            LogError("Mesh %s: out of memory for a %u byte display list", staticMesh->GetName().c_str(), allocSize);
            FreeMeshLists(lists, part);
            return nullptr;
        }

        // This invalidate is needed because the write-gather pipe does not use the cache.
        DCInvalidateRange(displayList, allocSize);
        GX_BeginDispList(displayList, allocSize);

        uint32_t at = 0;
        for (uint32_t k = 0; k < s.mStrips; ++k)
        {
            const uint32_t len = sStripLen[k];
            GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT0, uint16_t(len));
            for (uint32_t i = 0; i < len; ++i)
            {
                emit(sStrips[at + i]);
            }
            GX_End();
            at += len;
        }
        if (s.mSingles > 0)
        {
            GX_Begin(GX_TRIANGLES, GX_VTXFMT0, uint16_t(s.mSingles));
            for (uint32_t i = 0; i < s.mSingles; ++i)
            {
                emit(sSingles[i]);
            }
            GX_End();
        }

        out.mList = displayList;
        out.mSize = GX_EndDispList();
        OCT_ASSERT(out.mSize != 0);
        total += out.mSize;
    }

    outSize = total;
    return lists;
}

void GxMeshListsNoCull(void* displayList)
{
    MeshLists* lists = (MeshLists*)displayList;
    if (lists == nullptr)
    {
        return;
    }
    for (uint32_t i = 0; i < lists->mCount; ++i)
    {
        lists->mParts[i].mRadius = 1e30f;
    }
}

void DestroyMeshDisplayList(void* displayList)
{
    if (displayList != nullptr)
    {
        FreeMeshLists((MeshLists*)displayList, ((MeshLists*)displayList)->mCount);
    }
}

#endif
