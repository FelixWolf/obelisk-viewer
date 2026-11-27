#version 450
#extension GL_GOOGLE_include_directive : require
#define SMAA_VERTEX
#include "smaa.glsl"

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec2 outPixel;
layout(location = 2) out vec4 outOffset[3];

void main()
{
    outUv = smaaFullscreen();
    SMAABlendingWeightCalculationVS(outUv, outPixel, outOffset);
}
