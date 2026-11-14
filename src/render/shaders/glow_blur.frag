#version 450

// Glow, blurring (glowF.glsl): eight taps along one direction, weighted
// 0.25 0.5 0.8 1 1 0.8 0.5 0.25 and scaled by the strength; done across,
// then down, twice over.

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform sampler2D source;
layout(set = 3, binding = 0) uniform BlurFS
{
    vec4 params; // xy step (texture coordinates), z strength
};
layout(location = 0) out vec4 outColor;

void main()
{
    const float weights[8] = float[8](0.25, 0.5, 0.8, 1.0, 1.0, 0.8, 0.5, 0.25);
    vec4 sum = vec4(0.0);
    for(int i = 0; i < 8; i++)
    {
        sum += weights[i] * texture(source, inUv + params.xy * (float(i) - 3.5));
    }
    outColor = max(vec4(sum.rgb * params.z, sum.a), vec4(0.0));
}
