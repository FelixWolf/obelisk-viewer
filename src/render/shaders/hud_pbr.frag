#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// HUD attachments, PBR faces: unlit, as the official viewer's HUD PBR
// shader (pbropaqueF.glsl IS_HUD): base color plus emission, written in
// sRGB.

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUv;
layout(location = 4) flat in uint inDraw;

layout(set = 2, binding = 0) uniform sampler2D albedoMap;
layout(set = 2, binding = 1) uniform sampler2D emissiveMap;
layout(std430, set = 2, binding = 2) readonly buffer Draws
{
    DrawData draws[];
};

layout(location = 0) out vec4 outColor;

vec3 linearToSrgb(vec3 c)
{
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main()
{
    const vec4 baseColorFactor = draws[inDraw].material[0];
    const vec4 emissiveTint = draws[inDraw].material[1]; // RGB tint, A strength
    const float alphaCutoff = draws[inDraw].material[2].w;
    const vec4 baseColor = texture(albedoMap, inUv) * baseColorFactor;
    if(baseColor.a < alphaCutoff)
    {
        discard;
    }
    const vec3 color = baseColor.rgb +
                       texture(emissiveMap, inUv).rgb * emissiveTint.rgb * emissiveTint.a;
    outColor = vec4(linearToSrgb(clamp(color, 0.0, 1.0)), baseColor.a);
}
