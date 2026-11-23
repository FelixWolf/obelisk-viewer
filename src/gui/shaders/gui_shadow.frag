#version 450

// drop-shadow's shadow: the layer's coverage (moved by the offset, through
// the uv) in one color, nothing from outside the region.

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform sampler2D image;
layout(set = 3, binding = 0) uniform ShadowFS
{
    vec4 color;  // premultiplied
    vec4 limits; // uv: min xy, max zw
};
layout(location = 0) out vec4 outColor;

void main()
{
    const vec2 inside = step(limits.xy, inUv) * step(inUv, limits.zw);
    outColor = texture(image, inUv).a * inside.x * inside.y * color;
}
