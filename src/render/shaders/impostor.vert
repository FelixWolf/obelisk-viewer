#version 450

// An impostor's card (Scene::impostors): two triangles, no vertex buffer,
// facing where its picture was taken from. Corners in the picture's
// normalized device coordinates (x right, y up), passed on.

layout(set = 1, binding = 0) uniform ImpostorVS
{
    mat4 viewProjection;
    vec4 center; // where it's shown
    vec4 right;  // half extents
    vec4 up;
};
layout(location = 0) out vec2 outNdc;

void main()
{
    const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                    vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    const vec2 ndc = corners[gl_VertexIndex];
    outNdc = ndc;
    gl_Position = viewProjection * vec4(center.xyz + ndc.x * right.xyz + ndc.y * up.xyz, 1.0);
}
