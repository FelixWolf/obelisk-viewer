#version 450
#extension GL_GOOGLE_include_directive : require
#include "cubemap.glsl"

// A probe's irradiance, one face: the captured cube averaged over the
// hemisphere around each direction, weighted by the cosine (what a matte
// surface facing that way receives, over pi).

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform samplerCube capture;
layout(set = 3, binding = 0) uniform FilterFS
{
    vec4 params; // x face, z the capture's size, w its last mip level
};
layout(location = 0) out vec4 outColor;

const uint kSamples = 128u;

void main()
{
    const vec3 n = cubeFaceDirection(int(params.x), inUv);
    vec3 sum = vec3(0.0);
    for(uint i = 0u; i < kSamples; i++)
    {
        // Cosine-weighted directions: their plain average is the integral.
        const vec2 xi = hammersley(i, kSamples);
        const float phi = 2.0 * CUBE_PI * xi.x;
        const float cosTheta = sqrt(1.0 - xi.y);
        const float sinTheta = sqrt(xi.y);
        const vec3 l = aroundNormal(vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta), n);
        const float pdf = cosTheta / CUBE_PI;
        sum += textureLod(capture, l, sampleLevel(pdf, kSamples, params.z, params.w)).rgb;
    }
    outColor = vec4(sum / float(kSamples), 1.0);
}
