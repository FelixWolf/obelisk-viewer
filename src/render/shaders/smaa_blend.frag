#version 450
#extension GL_GOOGLE_include_directive : require
#include "smaa.glsl"

// SMAA's last pass: the view, each pixel blended with its neighbours by the
// weights, into the target.

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inOffset;
layout(set = 2, binding = 0) uniform sampler2D view;    // linear
layout(set = 2, binding = 1) uniform sampler2D weights; // linear
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(SMAANeighborhoodBlendingPS(inUv, inOffset, view, weights).rgb, 1.0);
}
