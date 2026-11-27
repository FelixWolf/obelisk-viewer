#version 450
#extension GL_GOOGLE_include_directive : require
#include "smaa.glsl"

// SMAA's second pass: how much each edge pixel blends with its neighbours.

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec2 inPixel;
layout(location = 2) in vec4 inOffset[3];
layout(set = 2, binding = 0) uniform sampler2D edges;  // linear
layout(set = 2, binding = 1) uniform sampler2D area;   // linear
layout(set = 2, binding = 2) uniform sampler2D search; // nearest
layout(location = 0) out vec4 outWeights;

void main()
{
    outWeights =
        SMAABlendingWeightCalculationPS(inUv, inPixel, inOffset, edges, area, search, vec4(0.0));
}
