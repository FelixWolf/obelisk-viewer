#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Classic terrain: four diffuse textures (a 4-layer array), projected
// straight down (like the official viewer's classic terrain), blended by
// the layer weights.

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inTexturePosition;
layout(location = 3) in vec2 inBlend;

layout(set = 2, binding = 0) uniform sampler2DArray diffuseMaps;
layout(set = 2, binding = 1) uniform sampler2D blendRamp;
layout(set = 2, binding = 2) uniform sampler2DArrayShadow shadowMap;

FRAME_FS_BLOCK

#include "atmosphere.glsl"
#include "shadow.glsl"
#define PROJECTORS_BINDING 3
#define LIGHTS_BINDING 4
#include "lights.glsl"

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 weights = terrainLayerWeights(blendRamp, inBlend);
    vec2 uv = inTexturePosition.xy;
    vec3 albedo = weights.x * texture(diffuseMaps, vec3(uv, 0.0)).rgb +
                  weights.y * texture(diffuseMaps, vec3(uv, 1.0)).rgb +
                  weights.z * texture(diffuseMaps, vec3(uv, 2.0)).rgb +
                  weights.w * texture(diffuseMaps, vec3(uv, 3.0)).rgb;

    vec3 n = normalize(inNormal);
    vec3 v = normalize(frame.cameraPosition.xyz - inWorldPosition);
    vec3 color = blinnPhong(n, v, frame.sunDirection.xyz, albedo, vec3(0.0), 1.0,
                            frame.sunColor.rgb) * sunShadow(shadowMap, inWorldPosition, n);
    color += albedo * hemisphereAmbient(n, frame.skyAmbient.rgb, frame.groundAmbient.rgb);
    color += albedo * localLight(inWorldPosition, n);
    outColor = vec4(applyAtmosphere(color, inWorldPosition), 1.0);
}
