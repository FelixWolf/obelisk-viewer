#version 450

// HDR scene -> display: exposure, then the official viewer's tonemap (ACES,
// mixed with the untouched color: RenderTonemapMix), then sRGB encoding (the
// swapchain is UNORM, so the shader does the encoding).

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform sampler2D hdrScene;
layout(set = 3, binding = 0) uniform TonemapFS
{
    vec4 settings; // x exposure, y how much of the tonemapped color (the rest linear)
};
layout(location = 0) out vec4 outColor;

// The official viewer's tonemapper (tonemapUtilF.glsl, RenderTonemapType 1):
// Stephen Hill's ACES fit, between ACES's input and output color matrices.
const mat3 kAcesInput = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823,
                             0.01566, 0.83777);
const mat3 kAcesOutput = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276,
                              -0.07367, -0.00605, 1.07602);

vec3 acesHill(vec3 color)
{
    color = kAcesInput * color;
    const vec3 a = color * (color + 0.0245786) - 0.000090537;
    const vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    return clamp(kAcesOutput * (a / b), 0.0, 1.0);
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
    const vec3 exposed = texture(hdrScene, inUv).rgb * settings.x;
    const vec3 color = clamp(mix(exposed, acesHill(exposed), settings.y), 0.0, 1.0);
    outColor = vec4(linearToSrgb(color) + dither() / 255.0, 1.0);
}
