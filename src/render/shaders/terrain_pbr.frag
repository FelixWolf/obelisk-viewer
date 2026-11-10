#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// PBR terrain: four PBR materials, each with the same maps as pbr.frag, stored
// as 4-layer arrays and blended by the per-vertex composition value.

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in float inComposition;

layout(set = 2, binding = 0) uniform sampler2DArray albedoMaps;
layout(set = 2, binding = 1) uniform sampler2DArray ormMaps;
layout(set = 2, binding = 2) uniform sampler2DArray emissiveMaps;
layout(set = 2, binding = 3) uniform sampler2DArray normalMaps;

FRAME_FS_BLOCK

layout(set = 3, binding = 1) uniform MaterialFS
{
    vec4 baseColorFactor[4];
    vec4 emissiveTint[4]; // RGB tint, A strength
    vec4 factors[4];      // x roughness, y metallic, z occlusion strength
} material;

layout(location = 0) out vec4 outColor;

struct Sample
{
    vec3 baseColor;
    vec3 orm;
    vec3 emissive;
    vec3 normal;
};

Sample sampleLayer(int layer)
{
    vec3 uvw = vec3(inUv, float(layer));
    Sample s;
    s.baseColor = texture(albedoMaps, uvw).rgb * material.baseColorFactor[layer].rgb;
    vec3 orm = texture(ormMaps, uvw).rgb;
    s.orm = vec3(mix(1.0, orm.r, material.factors[layer].z),
                 orm.g * material.factors[layer].x, orm.b * material.factors[layer].y);
    s.emissive = texture(emissiveMaps, uvw).rgb * material.emissiveTint[layer].rgb *
                 material.emissiveTint[layer].a;
    s.normal = decodeNormal(texture(normalMaps, uvw));
    return s;
}

void main()
{
    float c = clamp(inComposition, 0.0, 3.0);
    int lower = int(min(floor(c), 2.0));
    float blend = c - float(lower);
    Sample a = sampleLayer(lower);
    Sample b = sampleLayer(lower + 1);
    vec3 baseColor = mix(a.baseColor, b.baseColor, blend);
    vec3 orm = mix(a.orm, b.orm, blend);
    vec3 emissive = mix(a.emissive, b.emissive, blend);
    vec3 tangentNormal = normalize(mix(a.normal, b.normal, blend));

    // Texture space follows world X/Y, so the tangent is +X made orthogonal
    // to the surface normal.
    vec3 n = normalize(inNormal);
    vec3 t = normalize(vec3(1.0, 0.0, 0.0) - n * n.x);
    n = perturbNormal(n, vec4(t, 1.0), tangentNormal);

    float roughness = clamp(orm.g, 0.0, 1.0);
    float metallic = clamp(orm.b, 0.0, 1.0);
    vec3 v = normalize(frame.cameraPosition.xyz - inWorldPosition);
    vec3 color = pbrDirect(n, v, frame.sunDirection.xyz, baseColor, roughness, metallic,
                           frame.sunColor.rgb);
    color += orm.r * pbrAmbient(n, v, baseColor, roughness, metallic, frame.skyAmbient.rgb,
                                frame.groundAmbient.rgb);
    color += emissive;
    outColor = vec4(color, 1.0);
}
