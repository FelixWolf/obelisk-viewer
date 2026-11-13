#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Default (legacy) objects: diffuse, normal and specular maps, as the
// official viewer's legacy materials (materialF.glsl).
//   diffuse:  RGB color, A opacity, or how much shows unlit (emissive mask)
//   normal:   RGB tangent-space normal, A glossiness (scales the exponent)
//   specular: RGB specular color, A environment intensity
// Glossy faces reflect the probe softly, shiny ones (environment intensity)
// sharply (probes.glsl).
// The normal and specular maps have their own texture transforms.

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUv;
layout(location = 5) in vec2 inMeshUv;

layout(set = 2, binding = 0) uniform sampler2D diffuseMap;
layout(set = 2, binding = 1) uniform sampler2D normalMap;
layout(set = 2, binding = 2) uniform sampler2D specularMap;
layout(set = 2, binding = 3) uniform sampler2DArrayShadow shadowMap;

FRAME_FS_BLOCK

#include "atmosphere.glsl"
#include "shadow.glsl"
#define PROJECTORS_BINDING 4
#define PROBE_RADIANCE_BINDING 5
#define PROBE_IRRADIANCE_BINDING 6
#define BRDF_TABLE_BINDING 7
#define LIGHTS_BINDING 9
#define PROBES_BINDING 10
#include "lights.glsl"
#include "probes.glsl"

layout(location = 4) flat in uint inDraw;
layout(std430, set = 2, binding = 8) readonly buffer Draws
{
    DrawData draws[];
};

struct Material
{
    vec4 diffuseTint;  // RGB color, A opacity
    vec4 specularTint; // RGB color, A exponent where the normal map's A is 1
    vec4 options;      // x alpha cutoff (< 0: none), y fullbright (unlit) if > 0,
                       // z emissive mask if > 0, w opaque (alpha ignored) if > 0
};

vec2 mapUv(uint first)
{
    const vec3 uv = vec3(inMeshUv, 1.0);
    return vec2(dot(draws[inDraw].mapUv[first].xyz, uv),
                dot(draws[inDraw].mapUv[first + 1].xyz, uv));
}
Material material;

layout(location = 0) out vec4 outColor;

void main()
{
    material = Material(draws[inDraw].material[0], draws[inDraw].material[1],
                        draws[inDraw].material[2]);
    vec4 albedo = texture(diffuseMap, inUv) * material.diffuseTint;
    if(albedo.a < material.options.x)
    {
        discard;
    }
    // How much shows unlit: fullbright all of it; an emissive mask its alpha.
    const float emissive =
        max(material.options.y, material.options.z > 0.0 ? albedo.a : 0.0);
    if(material.options.w > 0.0)
    {
        albedo.a = 1.0;
    }
    if(emissive >= 1.0)
    {
        outColor = vec4(applyAtmosphere(albedo.rgb, inWorldPosition), albedo.a);
        return;
    }
    vec4 normalTexel = texture(normalMap, mapUv(0));
    vec3 n = perturbNormal(inNormal, inTangent, decodeNormal(normalTexel));
    if(!gl_FrontFacing)
    {
        n = -n;
    }
    vec3 v = normalize(frame.cameraPosition.xyz - inWorldPosition);
    vec3 l = frame.sunDirection.xyz;

    const vec4 specularTexel = texture(specularMap, mapUv(2));
    vec3 specularColor = specularTexel.rgb * material.specularTint.rgb;
    // Glossiness squared, as the official specular table (createLUTBuffers).
    float exponent = max(normalTexel.a * normalTexel.a * material.specularTint.a, 1.0);
    // The face's glossiness and environment intensity (the draw's mapUv w),
    // by the normal map's and the specular map's alpha.
    const float glossiness = draws[inDraw].mapUv[0].w * normalTexel.a;
    const float envIntensity = draws[inDraw].mapUv[1].w * specularTexel.a;

    const float shadow = sunShadow(shadowMap, inWorldPosition, gl_FrontFacing ? inNormal : -inNormal);
    vec3 color = blinnPhong(n, v, l, albedo.rgb, specularColor, exponent, frame.sunColor.rgb) * shadow;
    // Ambient light and reflections from the probes around.
    vec3 ambient = hemisphereAmbient(n, frame.skyAmbient.rgb, frame.groundAmbient.rgb);
    vec3 glossy = vec3(0.0);
    vec3 sharp = vec3(0.0);
    const bool reflections = anyProbes();
    if(reflections)
    {
        const ProbeLight light = sampleProbes(inWorldPosition, n, reflect(-v, n),
                                              1.0 - glossiness, envIntensity > 0.0, ambient);
        ambient = light.irradiance;
        glossy = light.radiance;
        sharp = light.sharp;
    }
    color += albedo.rgb * ambient;
    if(reflections && glossiness > 0.0)
    {
        applyGlossyReflection(color, glossy, n, v, specularColor, glossiness);
    }
    color = mix(color, albedo.rgb, emissive);
    if(reflections && envIntensity > 0.0)
    {
        applyShinyReflection(color, sharp, n, v, envIntensity);
    }
    // Local lights after (the official viewer adds them in later passes),
    // not on what shows unlit.
    color += (1.0 - emissive) * albedo.rgb * localLight(inWorldPosition, n);
    outColor = vec4(applyAtmosphere(color, inWorldPosition), albedo.a);
}
