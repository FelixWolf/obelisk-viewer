#version 450

// Multisampling: the scene's depth, one sample a pixel, for the passes after
// it, from the depth the scene pass wrote again as a color (setSceneTargets),
// resolved: the average of the pixel's samples. A surface drawn again at the
// pixel's middle (glow, the outline) is then at its depth there (depth is
// linear across a triangle), and an edge pixel is between its surfaces.

layout(set = 2, binding = 0) uniform sampler2D depthMap;

void main()
{
    gl_FragDepth = texelFetch(depthMap, ivec2(gl_FragCoord.xy), 0).r;
}
