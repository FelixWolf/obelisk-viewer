#version 450

// Dynamic exposure, step 1: the lit scene's luminance, into a 256x256
// map whose mip chain then averages it (as the official viewer's
// luminanceF.glsl). The middle of the view, nudged down to favor the
// ground over the sky; surfaces count at RenderDiffuseLuminanceScale (0.5),
// the sky in full, so bright skies don't darken what's under them.

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform sampler2D hdrScene;
layout(set = 2, binding = 1) uniform sampler2D sceneDepth;
layout(location = 0) out vec4 outLuminance;

void main()
{
    vec2 uv = inUv * 0.6 + 0.2;
    uv.y += 0.1; // v grows downwards here
    vec3 c = texture(hdrScene, uv).rgb;
    const ivec2 texel = ivec2(uv * vec2(textureSize(sceneDepth, 0)));
    if(texelFetch(sceneDepth, texel, 0).r < 1.0)
    {
        c *= 0.5; // a surface, not the sky
    }
    outLuminance = vec4(max(dot(vec3(0.2126, 0.7152, 0.0722), c), 0.0));
}
