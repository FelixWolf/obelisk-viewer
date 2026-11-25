#version 450

// Multisampling: the scene's depth, one sample a pixel, for the passes after
// it. The farthest of the pixel's: a surface drawn again at its middle (glow,
// the outline) is then never behind itself, however steep, and occlusion
// culling hides only what's behind all of it.

layout(set = 2, binding = 0) uniform sampler2DMS depthMap;

void main()
{
    const ivec2 p = ivec2(gl_FragCoord.xy);
    const int samples = textureSamples(depthMap);
    float farthest = 0.0;
    for(int i = 0; i < samples; i++)
    {
        farthest = max(farthest, texelFetch(depthMap, p, i).r);
    }
    gl_FragDepth = farthest;
}
