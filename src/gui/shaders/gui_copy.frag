#version 450

// A layer copied (premultiplied), times a factor (the opacity filter), its
// reads kept inside the region copied so nothing beside it bleeds in.

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform sampler2D image;
layout(set = 3, binding = 0) uniform CopyFS
{
    vec4 factor;
    vec4 limits; // uv: min xy, max zw
};
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(image, clamp(inUv, limits.xy, limits.zw)) * factor;
}
