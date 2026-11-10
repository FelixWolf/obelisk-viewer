#version 450
#extension GL_GOOGLE_include_directive : require

// Vertex shader for objects (Blinn-Phong and PBR).

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent; // xyz tangent, w bitangent sign (MikkTSpace)
layout(location = 3) in vec2 inUv;

layout(set = 1, binding = 0) uniform FrameVS
{
    mat4 viewProjection;
    vec4 cameraPosition;
};
layout(set = 1, binding = 1) uniform ObjectVS
{
    mat4 model;
    mat4 normalMatrix; // inverse transpose of model (upper 3x3 used)
};

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec4 outTangent;
layout(location = 3) out vec2 outUv;

void main()
{
    vec4 world = model * vec4(inPosition, 1.0);
    outWorldPosition = world.xyz;
    outNormal = mat3(normalMatrix) * inNormal;
    outTangent = vec4(mat3(model) * inTangent.xyz, inTangent.w);
    outUv = inUv;
    gl_Position = viewProjection * world;
}
