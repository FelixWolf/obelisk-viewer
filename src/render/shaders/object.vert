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
layout(location = 5) out vec2 outMeshUv; // before the texture transform (legacy maps have their own)

// Planar mapping's texture coordinates, as the official viewer's
// planarProjection (llface.cpp): the face's plane picked from its normal,
// two units per meter of the scaled position.
vec2 planarUv(vec3 normal, vec3 position)
{
    vec3 binormal;
    if(abs(normal.x) >= 0.5)
    {
        binormal = vec3(0.0, normal.x < 0.0 ? -1.0 : 1.0, 0.0);
    }
    else
    {
        binormal = vec3(normal.y > 0.0 ? -1.0 : 1.0, 0.0, 0.0);
    }
    const vec3 tangent = cross(binormal, normal);
    return vec2(1.0 + (dot(binormal, position) * 2.0 - 0.5),
                -(dot(tangent, position) * 2.0 - 0.5));
}

void main()
{
    const DrawData draw = draws[inDraw];
    vec4 world = draw.model * vec4(inPosition, 1.0);
    outWorldPosition = world.xyz;
    outNormal = mat3(draw.normalMatrix) * inNormal;
    outTangent = vec4(mat3(draw.model) * inTangent.xyz, inTangent.w);
    vec2 uv = inUv;
    if(draw.uvRow0.w > 0.5)
    {
        // The model matrix is the object's frame times its scale.
        const vec3 scale =
            vec3(length(draw.model[0].xyz), length(draw.model[1].xyz), length(draw.model[2].xyz));
        uv = planarUv(inNormal, inPosition * scale);
    }
    outUv = vec2(dot(draw.uvRow0.xyz, vec3(uv, 1.0)), dot(draw.uvRow1.xyz, vec3(uv, 1.0)));
    outMeshUv = uv;
    outDraw = inDraw;
    gl_Position = viewProjection * world;
}
