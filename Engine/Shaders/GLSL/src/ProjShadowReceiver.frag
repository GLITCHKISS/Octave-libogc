#version 450
#extension GL_ARB_separate_shader_objects : enable

// Projected shadows: a receiver drawn again over itself, the shadow texture laid straight down onto it from the
// casters' camera (projShadow.mViewProj: its group's, one draw a group), multiplying what is already there (the pipeline blends dst * src):
// white leaves it, the silhouette's grey darkens it. Outside the texture, its white border. The GX renderer's
// projected shadow is the reference: its multiply is on the 8-bit (sRGB) colour, so in linear colour space the
// grey is made linear first, to darken as much. Only what's no higher than mCut.x (its first caster's feet and a
// little): above that, white -- a block over a character's head is not under it.

#include "Common.glsl"

layout (set = 0, binding = 0) uniform GlobalUniformBuffer 
{
    GlobalUniforms global;
};

layout (set = 1, binding = 0) uniform GeometryUniformBuffer 
{
	GeometryUniforms geometry;
};

layout (set = 2, binding = 0) uniform sampler2D projShadowMap;

layout (set = 2, binding = 1) uniform ProjShadowUniformBuffer
{
    mat4 mViewProj;
    vec4 mCut;
} projShadow;

layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec4 outColor;

void main()
{
    if (inPosition.y > projShadow.mCut.x)
    {
        outColor = vec4(1.0);
        return;
    }

    vec4 p = projShadow.mViewProj * vec4(inPosition, 1.0);
    vec2 uv = p.xy * 0.5 + 0.5;
    vec4 shade = vec4(texture(projShadowMap, uv).rgb, 1.0);
    if (global.mLinearColorSpace != 0)
    {
        shade = SrgbToLinear(shade);
    }
    outColor = vec4(shade.rgb, 1.0);
}
