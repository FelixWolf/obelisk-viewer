#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Particles (RendererParticles.cpp): their texture times their color,
// blended as each asks. Emissive ones are unlit; the rest lit as the
// official viewer's alpha shader lights them, facing the camera: the sun
// (and its shadows), the ambient light and the local lights.

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;
layout(location = 4) flat in float inFlags; // 1 ribbon, 2 fullbright

layout(set = 2, binding = 0) uniform sampler2D particleMap;
layout(set = 2, binding = 1) uniform sampler2DArrayShadow shadowMap;

FRAME_FS_BLOCK

#define WATER_MASK_BINDING 3
#include "atmosphere.glsl"
#include "shadow.glsl"
#define PROJECTORS_BINDING 2
#define LIGHTS_BINDING 4
#define CLUSTERS_BINDING 5
#include "clusters.glsl"
#include "lights.glsl"

layout(location = 0) out vec4 outColor;

void main()
{
    const vec4 albedo = texture(particleMap, inUv) * inColor;
    const int flags = int(inFlags + 0.5);
    if((flags & 2) != 0)
    {
        outColor = vec4(applyAtmosphere(albedo.rgb, inWorldPosition), albedo.a);
        return;
    }
    const vec3 n = normalize(frame.cameraPosition.xyz - inWorldPosition);
    const vec3 l = frame.sunDirection.xyz;
    const float shadow = sunShadow(shadowMap, inWorldPosition, n);
    const vec3 sun = frame.sunColor.rgb * max(dot(n, l), 0.0) * shadow;
    const vec3 ambient =
        hemisphereAmbient(n, frame.skyAmbient.rgb, frame.groundAmbient.rgb) * ambientLighting(n, l);
    const vec3 color = albedo.rgb * (sun + ambient + localLight(inWorldPosition, n));
    outColor = vec4(applyAtmosphere(color, inWorldPosition), albedo.a);
}
