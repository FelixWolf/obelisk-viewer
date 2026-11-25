#version 450
#extension GL_GOOGLE_include_directive : require
// PBR objects (pbr_surface.glsl), opaque, with
// alpha to coverage (coverage.glsl).
#define ALPHA_TO_COVERAGE
#include "pbr_surface.glsl"
