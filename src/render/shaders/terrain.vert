#version 450

// Vertex shader for both terrain variants. Texture coordinates come from the
// world position, so textures tile seamlessly across regions.

layout(location = 0) in vec3 inPosition; // region-local meters
layout(location = 1) in vec3 inNormal;
layout(location = 2) in float inComposition; // 0..3: which detail texture

layout(set = 1, binding = 0) uniform FrameVS
{
    mat4 viewProjection;
    vec4 cameraPosition;
};
layout(set = 1, binding = 1) uniform TerrainVS
{
    mat4 model;        // region placement (translation only)
    vec4 textureScale; // x: texture repeats per meter
};

layout(location = 0) out vec3 outWorldPosition;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUv;
layout(location = 3) out float outComposition;

void main()
{
    vec4 world = model * vec4(inPosition, 1.0);
    outWorldPosition = world.xyz;
    outNormal = inNormal;
    outUv = world.xy * textureScale.x;
    outComposition = inComposition;
    gl_Position = viewProjection * world;
}
