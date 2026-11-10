#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Classic terrain: four diffuse textures (a 4-layer array), blended by the
// per-vertex composition value (0 = lowest layer ... 3 = highest).

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in float inComposition;

layout(set = 2, binding = 0) uniform sampler2DArray diffuseMaps;

FRAME_FS_BLOCK

layout(location = 0) out vec4 outColor;

void main()
{
    float c = clamp(inComposition, 0.0, 3.0);
    float lower = min(floor(c), 2.0);
    float blend = c - lower;
    vec3 albedo = mix(texture(diffuseMaps, vec3(inUv, lower)).rgb,
                      texture(diffuseMaps, vec3(inUv, lower + 1.0)).rgb, blend);

    vec3 n = normalize(inNormal);
    vec3 v = normalize(frame.cameraPosition.xyz - inWorldPosition);
    vec3 color = blinnPhong(n, v, frame.sunDirection.xyz, albedo, vec3(0.0), 1.0,
                            frame.sunColor.rgb);
    color += albedo * hemisphereAmbient(n, frame.skyAmbient.rgb, frame.groundAmbient.rgb);
    outColor = vec4(color, 1.0);
}
