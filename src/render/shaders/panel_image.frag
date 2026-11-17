#version 450

// A panel's image (an interface page: premultiplied, sRGB-encoded values,
// as the view's are by now), times its vertices' opacity.

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;
layout(set = 2, binding = 0) uniform sampler2D image;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(image, inUv) * inColor.a;
}
