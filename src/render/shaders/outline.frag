#version 450

// The outline around the hovered prim: pixels outside its mask with some
// of it within a few pixels, over the view.

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform sampler2D mask;
layout(set = 3, binding = 0) uniform OutlineFS
{
    vec4 color; // RGB (sRGB, as the view's output), A strength
    vec4 params; // x width in pixels
};
layout(location = 0) out vec4 outColor;

void main()
{
    const ivec2 size = textureSize(mask, 0);
    const ivec2 p = ivec2(inUv * vec2(size));
    if(texelFetch(mask, p, 0).r > 0.5)
    {
        discard;
    }
    const int width = int(params.x);
    float nearest = 1e9;
    for(int y = -width; y <= width; y++)
    {
        for(int x = -width; x <= width; x++)
        {
            const ivec2 q = clamp(p + ivec2(x, y), ivec2(0), size - 1);
            if(texelFetch(mask, q, 0).r > 0.5)
            {
                nearest = min(nearest, length(vec2(x, y)));
            }
        }
    }
    const float coverage = clamp(float(width) + 0.5 - nearest, 0.0, 1.0);
    if(coverage <= 0.0)
    {
        discard;
    }
    outColor = vec4(color.rgb, color.a * coverage);
}
