#version 450

// HDR scene -> display: exposure, ACES filmic curve, then sRGB encoding
// (the swapchain is UNORM, so the shader does the encoding).

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform sampler2D hdrScene;
layout(set = 3, binding = 0) uniform TonemapFS
{
    vec4 settings; // x exposure
};
layout(location = 0) out vec4 outColor;

vec3 acesFilm(vec3 x)
{
    // Narkowicz 2015 fit.
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 linearToSrgb(vec3 c)
{
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

// Noise of under one 8-bit step, so slow gradients (the sky) don't show
// bands when the display rounds them.
float dither()
{
    return fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5;
}

void main()
{
    vec3 hdr = texture(hdrScene, inUv).rgb * settings.x;
    outColor = vec4(linearToSrgb(acesFilm(hdr)) + dither() / 255.0, 1.0);
}
