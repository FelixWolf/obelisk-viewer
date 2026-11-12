#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Shadow maps for casters with see-through parts (alpha masked or
// blended): only the parts at least half opaque cast. The tint's alpha is
// material[0].a for both material families.

layout(location = 3) in vec2 inUv;
layout(location = 4) flat in uint inDraw;

layout(set = 2, binding = 0) uniform sampler2D diffuseMap;
layout(std430, set = 2, binding = 1) readonly buffer Draws
{
    DrawData draws[];
};

void main()
{
    if(texture(diffuseMap, inUv).a * draws[inDraw].material[0].a < 0.5)
    {
        discard;
    }
}
