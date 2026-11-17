#version 450

// Labels (Scene::labels), panels and pointer lines: quads already placed
// in clip space (labels' at whole pixels, with their label's depth).

layout(location = 0) in vec4 inPosition; // clip space
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor; // sRGB, alpha
layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;

void main()
{
    outUv = inUv;
    outColor = inColor;
    gl_Position = inPosition;
}
