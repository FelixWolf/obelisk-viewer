#version 450
#extension GL_GOOGLE_include_directive : require
#include "tonemap.glsl"

// HDR scene -> display: exposure (the setting times the dynamic exposure),
// then the official viewer's tonemap (ACES, mixed with the untouched color:
// RenderTonemapMix), then sRGB encoding (the swapchain is UNORM, so the
// shader does the encoding), then the glow added (glow_blur.frag).

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform sampler2D hdrScene;
layout(set = 2, binding = 1) uniform sampler2D exposureMap; // 1x1: the dynamic exposure
layout(set = 2, binding = 2) uniform sampler2D glowMap;     // the blurred glow (or black)
layout(set = 3, binding = 0) uniform TonemapFS
{
    vec4 settings; // x exposure, y how much of the tonemapped color (the rest linear)
};
layout(location = 0) out vec4 outColor;

// Noise of under one 8-bit step, so slow gradients (the sky) don't show
// bands when the display rounds them.
float dither()
{
    return fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5;
}

void main()
{
    const float exposure = settings.x * texelFetch(exposureMap, ivec2(0), 0).r;
    const vec3 color = displayColor(texture(hdrScene, inUv).rgb, exposure, settings.y);
    // Glow goes on top, as the official viewer's glow combine adds it.
    outColor = vec4(color + texture(glowMap, inUv).rgb + dither() / 255.0, 1.0);
}
