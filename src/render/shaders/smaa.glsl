// SMAA (Jimenez et al.'s Enhanced Subpixel Morphological Antialiasing, 1x):
// upstream's SMAA.hlsl compiled as GLSL, its settings from the renderer
// (Renderer::setPostAntialiasing, kSmaaPresets). Include with SMAA_VERTEX
// defined in the vertex shaders.
//
// Three passes over the tonemapped view (sRGB-encoded, so luma is
// perceptual): its edges, by luma (smaa_edges); how much each edge pixel
// blends with its neighbours, from the edges' shapes looked up in the area
// and search textures (smaa_weights); the view blended so (smaa_blend).

#ifdef SMAA_VERTEX
layout(set = 1, binding = 0) uniform SmaaSettings
#else
layout(set = 3, binding = 0) uniform SmaaSettings
#endif
{
    vec4 metrics;  // 1 / width, 1 / height, width, height
    // x the contrast taken as an edge, y the steps searched along one,
    // z diagonally (0: no diagonals), w how much corners are rounded (%)
    vec4 settings;
}
smaa;

#define SMAA_GLSL_4
#define SMAA_RT_METRICS smaa.metrics
#define SMAA_THRESHOLD smaa.settings.x
#define SMAA_MAX_SEARCH_STEPS smaa.settings.y
#define SMAA_MAX_SEARCH_STEPS_DIAG smaa.settings.z
#define SMAA_CORNER_ROUNDING smaa.settings.w
#ifdef SMAA_VERTEX
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 0
#else
#define SMAA_INCLUDE_VS 0
#define SMAA_INCLUDE_PS 1
#endif
#include "SMAA.hlsl"

#ifdef SMAA_VERTEX
// One triangle covering the screen (as fullscreen.vert); its texture
// coordinate, top left 0 as SMAA's.
vec2 smaaFullscreen()
{
    const vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
    return vec2(corner.x, 1.0 - corner.y);
}
#endif
