#version 450
#extension GL_ARB_separate_shader_objects : enable

// Projected shadows: the casters' silhouette, one flat grey (as dark as the world's shadow colour says: its colour, by
// its alpha), drawn into the shadow texture over white. The GX renderer's projected shadow is the reference.

#include "Common.glsl"

layout (set = 0, binding = 0) uniform GlobalUniformBuffer 
{
    GlobalUniforms global;
};

layout(location = 0) in vec2 inTexcoord;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 sc = global.mShadowColor;
    outColor = vec4(mix(vec3(1.0), sc.rgb, clamp(sc.a, 0.0, 1.0)), 1.0);
}
