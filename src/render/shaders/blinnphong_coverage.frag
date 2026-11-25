#version 450
#extension GL_GOOGLE_include_directive : require
// Default (legacy) objects (blinnphong_surface.glsl), opaque, with
// alpha to coverage (coverage.glsl).
#define ALPHA_TO_COVERAGE
#include "blinnphong_surface.glsl"
