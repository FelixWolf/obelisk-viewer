#version 450
#extension GL_GOOGLE_include_directive : require
#include "cubemap.glsl"

// The split-sum table for image-based specular light: for N.V (u) and
// roughness (v), the scale (R) and bias (G) to the Fresnel reflectance at
// normal incidence, integrated over GGX with Smith's visibility. Made once.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

const uint kSamples = 512u;

float smithG1(float nDotX, float k)
{
    return nDotX / (nDotX * (1.0 - k) + k);
}

void main()
{
    const float nDotV = max(inUv.x, 1e-3);
    const float roughness = inUv.y;
    const vec3 n = vec3(0.0, 0.0, 1.0);
    const vec3 v = vec3(sqrt(1.0 - nDotV * nDotV), 0.0, nDotV);
    // Image-based light's k (alpha over 2).
    const float k = roughness * roughness * 0.5;
    float scale = 0.0;
    float bias = 0.0;
    for(uint i = 0u; i < kSamples; i++)
    {
        const vec3 h = sampleGgx(hammersley(i, kSamples), roughness, n);
        const vec3 l = 2.0 * dot(v, h) * h - v;
        const float nDotL = max(l.z, 0.0);
        if(nDotL <= 0.0)
        {
            continue;
        }
        const float nDotH = max(h.z, 0.0);
        const float vDotH = max(dot(v, h), 0.0);
        const float g = smithG1(nDotV, k) * smithG1(nDotL, k);
        const float visibility = g * vDotH / max(nDotH * nDotV, 1e-4);
        const float fresnel = pow(1.0 - vDotH, 5.0);
        scale += (1.0 - fresnel) * visibility;
        bias += fresnel * visibility;
    }
    outColor = vec4(scale / float(kSamples), bias / float(kSamples), 0.0, 1.0);
}
