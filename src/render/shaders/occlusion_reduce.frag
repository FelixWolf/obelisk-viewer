#version 450

// Occlusion culling (RendererOcclusion.cpp): the view's depth made small,
// each texel the farthest depth in its block of the view.

layout(set = 2, binding = 0) uniform sampler2D depthMap;
layout(set = 3, binding = 0) uniform ReduceFS
{
    vec4 size; // xy this target's size
};
layout(location = 0) out vec4 outDepth;

void main()
{
    const ivec2 source = textureSize(depthMap, 0);
    const vec2 scale = vec2(source) / size.xy;
    const ivec2 p = ivec2(gl_FragCoord.xy);
    const ivec2 first = ivec2(floor(vec2(p) * scale));
    const ivec2 last = min(ivec2(ceil(vec2(p + 1) * scale)), source) - 1;
    float farthest = 0.0;
    for(int y = first.y; y <= last.y; y++)
    {
        for(int x = first.x; x <= last.x; x++)
        {
            farthest = max(farthest, texelFetch(depthMap, ivec2(x, y), 0).r);
        }
    }
    outDepth = vec4(farthest, 0.0, 0.0, 0.0);
}
