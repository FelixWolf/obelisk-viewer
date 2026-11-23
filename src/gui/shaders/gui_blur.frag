#version 450

// One direction of a Gaussian blur: seven taps, weights from the middle out;
// taps outside the region count as transparent.

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform sampler2D image;
layout(set = 3, binding = 0) uniform BlurFS
{
    vec4 texel;   // xy: one texel along the blur, in uv
    vec4 limits;  // uv: min xy, max zw
    vec4 weights; // the middle's, then each side's further out
};
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 color = vec4(0.0);
    for(int i = -3; i <= 3; i++)
    {
        const vec2 uv = inUv + float(i) * texel.xy;
        const vec2 inside = step(limits.xy, uv) * step(uv, limits.zw);
        color += texture(image, uv) * (inside.x * inside.y * weights[abs(i)]);
    }
    outColor = color;
}
