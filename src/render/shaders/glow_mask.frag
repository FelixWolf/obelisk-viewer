#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Glow (RendererGlow.cpp): how much each pixel glows, added up over the
// glowing faces in front (depth tested against the view). As the official
// viewer's: a legacy face's glow times its texture's alpha (emissiveF.glsl);
// a PBR face's times its brightest emissive channel (pbrglowF.glsl).

layout(location = 3) in vec2 inUv;
layout(location = 4) flat in uint inDraw;

layout(set = 2, binding = 0) uniform sampler2D baseMap;     // diffuse or albedo
layout(set = 2, binding = 1) uniform sampler2D emissiveMap; // PBR
layout(std430, set = 2, binding = 2) readonly buffer Draws
{
    DrawData draws[];
};

layout(location = 0) out vec4 outGlow;

void main()
{
    const DrawData d = draws[inDraw];
    const float glow = d.uvRow1.w;
    const vec4 base = texture(baseMap, inUv);
    float amount;
    if(d.mapUv[0].w > 0.5) // PBR
    {
        const float cutoff = d.material[2].w;
        if(cutoff >= 0.0 && base.a * d.material[0].a < cutoff)
        {
            discard;
        }
        // Both linear (the map is sampled as sRGB).
        const vec3 map = texture(emissiveMap, inUv).rgb;
        const vec3 emissive = d.material[1].rgb * map;
        amount = max(max(emissive.r, emissive.g), emissive.b) * glow;
    }
    else
    {
        amount = base.a * glow;
    }
    if(amount <= 0.0)
    {
        discard;
    }
    outGlow = vec4(amount, 0.0, 0.0, 0.0);
}
