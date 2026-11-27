#version 450

// An impostor's card (impostor.vert): its picture (lit HDR color, as the
// view's) where the picture has something, and that something's own depth:
// the picture's depth back to where it was (moved with the avatar since),
// then into this view. So it sits among what's around it in each eye,
// though flat.

layout(location = 0) in vec2 inNdc;
layout(set = 2, binding = 0) uniform sampler2DArray bigColor;
layout(set = 2, binding = 1) uniform sampler2DArray bigDepth;
layout(set = 2, binding = 2) uniform sampler2DArray smallColor;
layout(set = 2, binding = 3) uniform sampler2DArray smallDepth;
layout(set = 3, binding = 0) uniform ImpostorFS
{
    mat4 inverseCapture; // the picture's view-projection, inverted
    mat4 viewProjection; // this view's
    vec4 offset;         // xyz: how far the avatar moved since; w: unused
    vec4 slot;           // x: layer, y: 1 big, 0 small
};
layout(location = 0) out vec4 outColor;
// Multisampled views: the depth again as a color (setSceneTargets).
layout(location = 1) out float outDepth;

void main()
{
    const vec3 uv = vec3(inNdc.x * 0.5 + 0.5, 0.5 - inNdc.y * 0.5, slot.x);
    const bool big = slot.y > 0.5;
    // The nearest texel's depth (filtered, an edge's would be between the
    // avatar and nothing).
    const ivec2 size = big ? textureSize(bigDepth, 0).xy : textureSize(smallDepth, 0).xy;
    const ivec3 texel = ivec3(clamp(ivec2(uv.xy * vec2(size)), ivec2(0), size - 1), int(slot.x));
    const float depth = big ? texelFetch(bigDepth, texel, 0).r : texelFetch(smallDepth, texel, 0).r;
    if(depth >= 1.0)
    {
        discard; // nothing there
    }
    const vec4 world = inverseCapture * vec4(inNdc, depth, 1.0);
    const vec4 clip = viewProjection * vec4(world.xyz / world.w + offset.xyz, 1.0);
    const float z = clamp(clip.z / clip.w, 0.0, 1.0);
    gl_FragDepth = z;
    outDepth = z;
    const vec3 color = big ? texture(bigColor, uv).rgb : texture(smallColor, uv).rgb;
    outColor = vec4(color, 1.0);
}
