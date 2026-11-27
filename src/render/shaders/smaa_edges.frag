#version 450
#extension GL_GOOGLE_include_directive : require
#include "smaa.glsl"

// SMAA's first pass: the view's edges (r left, g top), by luma.

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inOffset[3];
layout(set = 2, binding = 0) uniform sampler2D view;
layout(location = 0) out vec4 outEdges;

void main()
{
    outEdges = vec4(SMAALumaEdgeDetectionPS(inUv, inOffset, view), 0.0, 0.0);
}
