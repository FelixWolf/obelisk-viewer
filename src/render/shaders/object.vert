#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Vertex shader for objects (Blinn-Phong and PBR).

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent; // xyz tangent, w bitangent sign (MikkTSpace)
layout(location = 3) in vec2 inUv;
layout(location = 4) in uint inDraw; // per instance: this draw's index

layout(std430, set = 0, binding = 0) readonly buffer Draws
{
    DrawData draws[];
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

void main()
{
    const DrawData draw = draws[inDraw];
    vec4 world = draw.model * vec4(inPosition, 1.0);
    outWorldPosition = world.xyz;
    outNormal = mat3(draw.normalMatrix) * inNormal;
    outTangent = vec4(mat3(draw.model) * inTangent.xyz, inTangent.w);
    outUv = vec2(dot(draw.uvRow0.xyz, vec3(inUv, 1.0)), dot(draw.uvRow1.xyz, vec3(inUv, 1.0)));
    outDraw = inDraw;
    gl_Position = viewProjection * world;
}
