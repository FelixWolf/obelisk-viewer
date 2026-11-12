#version 450

// Dynamic exposure, step 2: the scene's average luminance (the
// luminance map's 1x1 level) picks an exposure between the sky's bounds,
// brighter for darker scenes, which last frame's eases towards (as the
// official viewer's exposureF.glsl). One pixel.

layout(set = 2, binding = 0) uniform sampler2D luminanceMap;
layout(set = 2, binding = 1) uniform sampler2D previousExposure;
layout(set = 3, binding = 0) uniform ExposureFS
{
    vec4 params;  // x the luminance taken as bright, y least exposure, z most, w speed error
    vec4 params2; // x seconds since last frame, y 1: last frame's is valid, z speed target
};
layout(location = 0) out vec4 outExposure;

void main()
{
    float l = textureLod(luminanceMap, vec2(0.5), 8.0).r;
    l = clamp(l, 0.0, params.x) / params.x;
    l *= l;
    float exposure = mix(params.z, params.y, l);
    if(params2.y > 0.5)
    {
        const float previous = texelFetch(previousExposure, ivec2(0), 0).r;
        const float speed = -log(params.w) / params2.z;
        exposure = mix(previous, exposure, 1.0 - exp(-speed * params2.x));
    }
    outExposure = vec4(max(exposure, 0.0));
}
