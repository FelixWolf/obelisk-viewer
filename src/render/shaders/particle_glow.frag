#version 450

// Particles' glow (RendererGlow.cpp): how much each glows, times how much
// of it shows (its texture's and color's alpha), added to the glow mask.

layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;
layout(location = 3) in float inGlow;

layout(set = 2, binding = 0) uniform sampler2D particleMap;

layout(location = 0) out vec4 outGlow;

void main()
{
    const float amount = inGlow * texture(particleMap, inUv).a * inColor.a;
    if(amount <= 0.0)
    {
        discard;
    }
    outGlow = vec4(amount, 0.0, 0.0, 0.0);
}
