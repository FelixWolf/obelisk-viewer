#version 450

// Water (RendererWater.cpp): squares of water in scene coordinates.

layout(location = 0) in vec3 inPosition;

layout(set = 1, binding = 0) uniform FrameVS
{
    mat4 viewProjection;
    vec4 cameraPosition;
};

layout(location = 0) out vec3 outWorldPosition;

void main()
{
    outWorldPosition = inPosition;
    gl_Position = viewProjection * vec4(inPosition, 1.0);
}
