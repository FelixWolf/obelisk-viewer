#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// PBR objects (glTF metallic-roughness).
//   albedo:   RGB base color (sRGB), A opacity
//   orm:      R ambient occlusion, G roughness, B metallic
//   emissive: RGB emission (sRGB), multiplied by the tint
//   normal:   MikkTSpace tangent-space normal

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUv;

layout(set = 2, binding = 0) uniform sampler2D albedoMap;
layout(set = 2, binding = 1) uniform sampler2D ormMap;
layout(set = 2, binding = 2) uniform sampler2D emissiveMap;
layout(set = 2, binding = 3) uniform sampler2D normalMap;
layout(set = 2, binding = 4) uniform sampler2DArrayShadow shadowMap;

FRAME_FS_BLOCK

#define WATER_MASK_BINDING 9
#include "atmosphere.glsl"
#include "shadow.glsl"
#define PROJECTORS_BINDING 5
#define PROBE_RADIANCE_BINDING 6
#define PROBE_IRRADIANCE_BINDING 7
#define BRDF_TABLE_BINDING 8
#define LIGHTS_BINDING 11
#define PROBES_BINDING 12
#define CLUSTERS_BINDING 13
#include "clusters.glsl"
#include "lights.glsl"
#include "probes.glsl"

layout(location = 4) flat in uint inDraw;
layout(std430, set = 2, binding = 10) readonly buffer Draws
{
    DrawData draws[];
};

struct Material
{
    vec4 baseColorFactor;
    vec4 emissiveTint; // RGB tint, A strength
    vec4 factors;      // x roughness, y metallic, z occlusion strength, w alpha cutoff (< 0: none)
};
Material material;

layout(location = 0) out vec4 outColor;

void main()
{
    material = Material(draws[inDraw].material[0], draws[inDraw].material[1],
                        draws[inDraw].material[2]);
    vec4 baseColor = texture(albedoMap, inUv) * material.baseColorFactor;
    if(baseColor.a < material.factors.w)
    {
        discard;
    }
    vec3 orm = texture(ormMap, inUv).rgb;
    float occlusion = mix(1.0, orm.r, material.factors.z);
    float roughness = clamp(orm.g * material.factors.x, 0.0, 1.0);
    float metallic = clamp(orm.b * material.factors.y, 0.0, 1.0);

    vec3 n = perturbNormal(inNormal, inTangent, decodeNormal(texture(normalMap, inUv)));
    if(!gl_FrontFacing)
    {
        n = -n;
    }
    vec3 v = normalize(frame.cameraPosition.xyz - inWorldPosition);

    const float shadow = sunShadow(shadowMap, inWorldPosition, gl_FrontFacing ? inNormal : -inNormal);
    const vec3 l = frame.sunDirection.xyz;
    const float amb = ambientLighting(n, l);
    vec3 color;
    if(classicMode())
    {
        color = classicPbr(n, v, l, baseColor.rgb, roughness, metallic, frame.skyAmbient.rgb * amb,
                           shadow) +
                occlusion * pbrProbeSpecular(inWorldPosition, n, v, baseColor.rgb, roughness,
                                             metallic);
    }
    else
    {
        color = pbrDirect(n, v, l, baseColor.rgb, roughness, metallic, frame.sunColor.rgb) * shadow;
        color += occlusion * pbrProbeLight(inWorldPosition, n, v, baseColor.rgb, roughness,
                                           metallic, frame.skyAmbient.rgb * amb,
                                           frame.groundAmbient.rgb * amb);
    }
    color += texture(emissiveMap, inUv).rgb * material.emissiveTint.rgb * material.emissiveTint.a;
    color *= classicScale();
    // Local lights: diffuse, and GGX highlights.
    const LocalLighting local = localLights(inWorldPosition, n, v, true, 0.0,
                                            mix(vec3(0.04), baseColor.rgb, metallic), roughness);
    color += (baseColor.rgb * (1.0 - metallic) * local.diffuse + local.specular) *
             classicLocalScale();
    outColor = vec4(applyAtmosphere(color, inWorldPosition), baseColor.a);
}
