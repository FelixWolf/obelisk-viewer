// The display transform shared by tonemap.frag and glow_extract.frag:
// exposure, the official viewer's tonemap (ACES, mixed with the untouched
// color: RenderTonemapMix), then sRGB encoding.

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

// An HDR color as the display shows it (sRGB-encoded): `exposure` the
// setting times the dynamic exposure, `tonemapped` how much of the
// tonemapped color (the rest linear).
vec3 displayColor(vec3 hdr, float exposure, float tonemapped)
{
    const vec3 exposed = hdr * exposure;
    return linearToSrgb(clamp(mix(exposed, acesHill(exposed), tonemapped), 0.0, 1.0));
}
