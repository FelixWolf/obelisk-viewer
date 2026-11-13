#version 450

// The HUD image on a panel in the scene: two triangles, no vertex buffer.
// The panel faces -X of its frame, +Y left and +Z up (HUD space).

layout(set = 1, binding = 0) uniform PanelVS
{
    mat4 viewProjection;
    mat4 panel;   // its frame
    vec4 size;    // xy width and height, meters
};
layout(location = 0) out vec2 outUv;
layout(location = 1) out vec3 outWorldPosition;

void main()
{
    const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0),
                                    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(1.0, 0.0));
    const vec2 uv = corners[gl_VertexIndex];
    const vec3 local = vec3(0.0, (0.5 - uv.x) * size.x, (0.5 - uv.y) * size.y);
    const vec4 world = panel * vec4(local, 1.0);
    outUv = uv;
    outWorldPosition = world.xyz;
    gl_Position = viewProjection * world;
}
