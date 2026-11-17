#version 450

// Untextured RmlUi geometry: its vertices' colors (premultiplied).

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = inColor;
}
