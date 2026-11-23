#version 450

// A rectangle of one image drawn into another (layers, filters): no vertex
// buffer, its corners from the vertex's index.

layout(set = 1, binding = 0) uniform QuadVS
{
    vec4 dst; // x0, y0 (top left), x1, y1 (bottom right), clip space
    vec4 src; // the same corners in the source image (uv)
};
layout(location = 0) out vec2 outUv;

void main()
{
    const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
    const vec2 corner = corners[gl_VertexIndex];
    outUv = mix(src.xy, src.zw, corner);
    gl_Position = vec4(mix(dst.xy, dst.zw, corner), 0.0, 1.0);
}
