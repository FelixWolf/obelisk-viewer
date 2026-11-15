#version 450

// Water exclusion surfaces into the water mask (water_mask.frag).

layout(location = 0) in vec3 inPosition;

layout(set = 1, binding = 0) uniform FrameVS
{
    mat4 viewProjection;
    vec4 cameraPosition;
};
layout(set = 1, binding = 1) uniform ModelVS
{
    mat4 model;
};

layout(location = 0) out vec3 outWorldPosition;

void main()
{
    const vec4 world = model * vec4(inPosition, 1.0);
    outWorldPosition = world.xyz;
    gl_Position = viewProjection * world;
}
