#version 450

// Textured RmlUi geometry (images, glyphs): the texture (premultiplied)
// times the vertices' colors.

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;
layout(set = 2, binding = 0) uniform sampler2D image;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = inColor * texture(image, inUv);
}
