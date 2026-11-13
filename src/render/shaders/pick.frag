#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Picking (RendererPick.cpp): one pixel looking down a ray. The nearest face
// writes what was hit there: its pick id and the surface, as the official
// viewer's pick reports it (texture coordinates after the face's repeats,
// the face's own coordinates, position, normal, binormal). See-through
// texels don't count, except on faces drawn only to be picked (invisible
// prims that can be clicked).

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUv;
layout(location = 4) flat in uint inDraw;
layout(location = 5) in vec2 inMeshUv;

layout(set = 2, binding = 0) uniform sampler2D baseMap; // diffuse or albedo
layout(std430, set = 2, binding = 1) readonly buffer Draws
{
    DrawData draws[];
};

layout(location = 0) out uvec4 outId;      // xy pick id (low, high), z the face's t
layout(location = 1) out vec4 outPosition; // xyz, w texture u
layout(location = 2) out vec4 outNormal;   // xyz, w texture v
layout(location = 3) out vec4 outBinormal; // xyz, w the face's s

void main()
{
    const uvec4 skin = draws[inDraw].skin; // y, z pick id; w 1: drawn only to be picked
    if(skin.w == 0u)
    {
        const float alpha = texture(baseMap, inUv).a * draws[inDraw].material[0].a;
        if(alpha < 0.05)
        {
            discard;
        }
    }
    const vec3 n = normalize(inNormal);
    const vec3 t = normalize(inTangent.xyz - n * dot(n, inTangent.xyz));
    const vec3 b = cross(n, t) * (inTangent.w < 0.0 ? -1.0 : 1.0);
    outId = uvec4(skin.y, skin.z, floatBitsToUint(inMeshUv.y), 1u);
    outPosition = vec4(inWorldPosition, inUv.x);
    outNormal = vec4(n, inUv.y);
    outBinormal = vec4(b, inMeshUv.x);
}
