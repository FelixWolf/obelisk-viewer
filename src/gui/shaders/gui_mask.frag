#version 450

// mask-image: a layer kept where the saved mask covers it.

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform sampler2D image;
layout(set = 2, binding = 1) uniform sampler2D mask;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(image, inUv) * texture(mask, inUv).a;
}
