#version 450
#extension GL_GOOGLE_include_directive : require
#include "cubemap.glsl"

// A probe's radiance, one face of one mip level: the captured cube seen
// through a GGX lobe of this level's roughness (level / last level), so
// rougher surfaces read blurrier levels. Level 0 is the capture itself.

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform samplerCube capture;
layout(set = 3, binding = 0) uniform FilterFS
{
    vec4 params; // x face, y roughness, z the capture's size, w its last mip level
};
layout(location = 0) out vec4 outColor;

const uint kSamples = 64u;

void main()
{
    const vec3 n = cubeFaceDirection(int(params.x), inUv);
    const float roughness = params.y;
    if(roughness <= 0.0)
    {
        outColor = vec4(textureLod(capture, n, 0.0).rgb, 1.0);
        return;
    }
    // View along the normal (the usual split-sum assumption).
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for(uint i = 0u; i < kSamples; i++)
    {
        const vec3 h = sampleGgx(hammersley(i, kSamples), roughness, n);
        const vec3 l = 2.0 * dot(n, h) * h - n;
        const float nDotL = dot(n, l);
        if(nDotL <= 0.0)
        {
            continue;
        }
        const float nDotH = max(dot(n, h), 0.0);
        // With v = n, the pdf over l is D(h) / 4.
        const float pdf = ggxDistribution(nDotH, roughness) * 0.25 + 1e-4;
        const float level = sampleLevel(pdf, kSamples, params.z, params.w);
        sum += textureLod(capture, l, level).rgb * nDotL;
        weight += nDotL;
    }
    outColor = vec4(sum / max(weight, 1e-4), 1.0);
}
