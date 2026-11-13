#version 450

// The HUD image over the view: it's premultiplied by its coverage (drawn
// into a transparent image) and sRGB-encoded, as the view's output is.

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform sampler2D hud;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(hud, inUv);
}
