#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Vertex shader for skinned objects (avatar bodies, rigged mesh): each
// vertex is a weighted blend of up to four joint matrices, which take it
// from where it was modeled to where the joint is now (relative to `model`).

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent; // xyz tangent, w bitangent sign (MikkTSpace)
layout(location = 3) in vec2 inUv;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4 inWeights;
layout(location = 6) in uint inDraw; // per instance: this draw's index

layout(std430, set = 0, binding = 0) readonly buffer Draws
{
    DrawData draws[];
};
layout(std430, set = 0, binding = 1) readonly buffer Joints
{
    mat4 joints[];
};

layout(set = 1, binding = 0) uniform FrameVS
{
    mat4 viewProjection;
    vec4 cameraPosition;
};

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec4 outTangent;
layout(location = 3) out vec2 outUv;
layout(location = 4) flat out uint outDraw;
layout(location = 5) out vec2 outMeshUv; // before the texture transform (legacy maps have their own)

void main()
{
    const DrawData draw = draws[inDraw];
    const uint base = draw.skin.x;
    const mat4 skinning = inWeights.x * joints[base + inJoints.x] +
                          inWeights.y * joints[base + inJoints.y] +
                          inWeights.z * joints[base + inJoints.z] +
                          inWeights.w * joints[base + inJoints.w];
    const mat4 world = draw.model * skinning;
    // Relative to the draw's frame (the skeleton's root): small numbers,
    // precise. Projected from there with one matrix for the whole draw, not
    // from the world position: world coordinates are region sized, and their
    // rounding (hundredths of a millimetre, different for each vertex) is as
    // much as the gap between layers made to lie on each other (fur on a
    // body), which then poke through each other as the view turns. The same
    // matrix for every draw on the same skeleton rounds them all alike.
    const vec4 local = skinning * vec4(inPosition, 1.0);
    const vec4 position = draw.model * local;
    outWorldPosition = position.xyz;
    // Normals need the inverse transpose: the bind shape matrix stretches
    // mesh (a long tail, a flat plate), and stretched normals lean the
    // wrong way (the official viewer applies the bind shape's inverse
    // transpose to normals, llface.cpp). The cofactor matrix is that up to
    // a factor (the determinant, whose sign matters if mirrored).
    const mat3 m = mat3(world);
    const mat3 cofactor = mat3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1]));
    outNormal = cofactor * inNormal * sign(determinant(m));
    outTangent = vec4(mat3(world) * inTangent.xyz, inTangent.w);
    outUv = vec2(dot(draw.uvRow0.xyz, vec3(inUv, 1.0)), dot(draw.uvRow1.xyz, vec3(inUv, 1.0)));
    outMeshUv = inUv;
    outDraw = inDraw;
    gl_Position = (viewProjection * draw.model) * local;
}
