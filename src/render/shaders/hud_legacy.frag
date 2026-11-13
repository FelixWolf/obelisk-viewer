#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// HUD attachments, legacy faces: unlit (the official viewer draws HUDs
// fullbright), the diffuse texture times the face color, in sRGB as the
// official HUD shaders write it (blended in sRGB, after tonemapping).

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUv;
layout(location = 4) flat in uint inDraw;

layout(set = 2, binding = 0) uniform sampler2D diffuseMap;
layout(std430, set = 2, binding = 1) readonly buffer Draws
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
    const vec4 tint = draws[inDraw].material[0];
    const vec4 options = draws[inDraw].material[2]; // x alpha cutoff, w opaque
    vec4 texel = texture(diffuseMap, inUv);
    float alpha = texel.a * tint.a;
    if(alpha < options.x)
    {
        discard;
    }
    if(options.w > 0.0)
    {
        alpha = 1.0;
    }
    outColor = vec4(linearToSrgb(texel.rgb) * tint.rgb, alpha);
}
