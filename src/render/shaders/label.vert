#version 450

// Labels (Scene::labels): glyph quads already placed in clip space, at
// whole pixels, with their label's depth.

layout(location = 0) in vec3 inPosition; // x, y clip space; z depth
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor; // sRGB, alpha
layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;

void main()
{
    outUv = inUv;
    outColor = inColor;
    gl_Position = vec4(inPosition, 1.0);
}
