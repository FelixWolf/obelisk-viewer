#version 450

// A page's image (premultiplied, sRGB-encoded as the window's values are).

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform sampler2D page;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(page, inUv);
}
