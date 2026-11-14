#version 450
#extension GL_GOOGLE_include_directive : require
#include "tonemap.glsl"

// Glow, first step (glowExtractF.glsl): the view as the display will show
// it, times how much each pixel glows (glow_mask.frag, at most 1), into the
// small glow image: color times glow, and glow squared (the official
// additive-with-alpha blend onto black).

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform sampler2D hdrScene;
layout(set = 2, binding = 1) uniform sampler2D exposureMap;
layout(set = 2, binding = 2) uniform sampler2D glowMask;
layout(set = 3, binding = 0) uniform GlowFS
{
    vec4 settings; // as tonemap.frag's
};
layout(location = 0) out vec4 outColor;

void main()
{
    const float glow = clamp(texture(glowMask, inUv).r, 0.0, 1.0);
    const float exposure = settings.x * texelFetch(exposureMap, ivec2(0), 0).r;
    const vec3 color = displayColor(texture(hdrScene, inUv).rgb, exposure, settings.y);
    outColor = vec4(color * glow, glow * glow);
}
