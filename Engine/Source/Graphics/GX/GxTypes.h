#pragma once

#if API_GX

#include <gccore.h>
#include <stdint.h>
#include "EngineTypes.h"

class World;

struct LightingState
{
    bool mEnabled = false;
    bool mColorChannel = false;
    uint8_t mMaterialSrc = GX_SRC_REG;
    uint8_t mLightMask = 0;
    uint8_t mDiffuseFunc = GX_DF_CLAMP;
    uint8_t mAttenuationFunc = GX_AF_SPOT;
    uint8_t mSpecularMask = 0;   // (not 0: COLOR1 is the material's specular, lit by this light alone)

    LightingState()
    {

    }

    LightingState(bool enable, bool colorChannel, uint8_t matSrc, uint8_t lightMask, uint8_t diffFunc, uint8_t attenFunc) :
        mEnabled(enable),
        mColorChannel(colorChannel),
        mMaterialSrc(matSrc),
        mLightMask(lightMask),
        mDiffuseFunc(diffFunc),
        mAttenuationFunc(attenFunc)
    {

    }

    bool operator==(const LightingState& other) const
    {
        return
            mEnabled == other.mEnabled &&
            mColorChannel == other.mColorChannel &&
            mMaterialSrc == other.mMaterialSrc &&
            mLightMask == other.mLightMask &&
            mDiffuseFunc == other.mDiffuseFunc &&
            mAttenuationFunc == other.mAttenuationFunc &&
            mSpecularMask == other.mSpecularMask;
    }

    bool operator!=(const LightingState& other) const
    {
        return !operator==(other);
    }
};

struct GxContext
{
    void* mGpFifo = nullptr;
    World* mWorld = nullptr;

    // Lighting/Channel control
    LightingState mLighting;
    uint8_t mSceneLightMask = 0;
    uint8_t mSceneNumLights = 0;
    LightData mLightData[MAX_LIGHTS_PER_DRAW];

    // The first directional light, for specular: its direction in view space and its colour.
    bool mHasSpecLight = false;
    glm::vec3 mSpecDirVS = glm::vec3(0.0f, 0.0f, -1.0f);
    GXColor mSpecColor = { 0, 0, 0, 255 };
    glm::vec3 mSpecHalfVS = glm::vec3(0.0f, 0.0f, 1.0f);   // (its half vector with the view, in view space)
    bool mSpecTexBound = false;                             // (the bound material samples the lobe texture)
    glm::vec3 mObjectViewPos = glm::vec3(0.0f);             // (the object being drawn, its origin in view space)
    // Each scene light's direction in view space and its colour (the sharp specular's: the lights that light
    // the draw, by its lighting channels -- SetupLightMask), and the bound material's specular x its colour.
    glm::vec3 mLightDirVS[MAX_LIGHTS_PER_DRAW];
    GXColor mLightGxColor[MAX_LIGHTS_PER_DRAW];
    glm::vec3 mSpecMatColor = glm::vec3(0.0f);
    Mtx mSpecNormalMtx;
    float mSpecNormalScale = 1.0f;

    // Fog
    bool mApplyFog = true;
    uint8_t mFogType = 0;
    float mFogStartZ = 0.0f;
    float mFogEndZ = 0.0f;
    float mFogNearZ = 0.0f;
    float mFogFarZ = 0.0f;
    GXColor mFogColor = {0,0,0,0};

    // Color Scale
    float mColorScale = 1.0f;
    float mInvColorScale = 1.0f;
    uint8_t mColorScaleEnum = GX_CS_SCALE_1;

    // The material BindMaterial last set has a UV_MAP_ENVIRONMENT slot (LoadEnvTexMtx).
    bool mEnvMapBound = false;

    // TEV stages BindMaterial left reading the indirect (warp) stage, one bit per stage. Non-zero
    // also means GX_SetNumIndStages(1) is in effect. GxResetIndirect() turns it all off.
    uint32_t mIndirectTevMask = 0;
};

#endif