#version 450

// A page's image over the window: a rectangle (clip space), no vertex
// buffer.

layout(set = 1, binding = 0) uniform BlitVS
{
    vec4 rect; // x0, y0 (top left), x1, y1 (bottom right), clip space
};
layout(location = 0) out vec2 outUv;

void main()
{
    const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
    const vec2 uv = corners[gl_VertexIndex];
    outUv = uv;
    gl_Position = vec4(mix(rect.xy, rect.zw, uv), 0.0, 1.0);
}
