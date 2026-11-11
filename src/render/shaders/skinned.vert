#version 450
#extension GL_GOOGLE_include_directive : require

// Vertex shader for skinned objects (avatar bodies, rigged mesh): each
// vertex is a weighted blend of up to four joint matrices, which take it
// from where it was modeled to where the joint is now (relative to `model`).

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent; // xyz tangent, w bitangent sign (MikkTSpace)
layout(location = 3) in vec2 inUv;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4 inWeights;

layout(std430, set = 0, binding = 0) readonly buffer Joints
{
    mat4 joints[];
};

layout(set = 1, binding = 0) uniform FrameVS
{
    mat4 viewProjection;
    vec4 cameraPosition;
};
layout(set = 1, binding = 1) uniform ObjectVS
{
    mat4 model;
    mat4 normalMatrix; // inverse transpose of model (upper 3x3 used)
    vec4 uvRow0;
    vec4 uvRow1;
};
layout(set = 1, binding = 2) uniform SkinVS
{
    uvec4 skin; // x: this draw's first matrix in `joints`
};

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec4 outTangent;
layout(location = 3) out vec2 outUv;

void main()
{
    const uint base = skin.x;
    const mat4 skinning = inWeights.x * joints[base + inJoints.x] +
                          inWeights.y * joints[base + inJoints.y] +
                          inWeights.z * joints[base + inJoints.z] +
                          inWeights.w * joints[base + inJoints.w];
    const mat4 world = model * skinning;
    const vec4 position = world * vec4(inPosition, 1.0);
    outWorldPosition = position.xyz;
    // Joints are rotations with near-uniform scale, so the matrix itself
    // carries normals well enough.
    outNormal = mat3(world) * inNormal;
    outTangent = vec4(mat3(world) * inTangent.xyz, inTangent.w);
    outUv = vec2(dot(uvRow0.xyz, vec3(inUv, 1.0)), dot(uvRow1.xyz, vec3(inUv, 1.0)));
    gl_Position = viewProjection * position;
}
