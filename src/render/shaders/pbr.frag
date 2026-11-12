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

FRAME_FS_BLOCK

#include "atmosphere.glsl"

layout(location = 4) flat in uint inDraw;
layout(std430, set = 2, binding = 4) readonly buffer Draws
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

    vec3 color = pbrDirect(n, v, frame.sunDirection.xyz, baseColor.rgb, roughness, metallic,
                           frame.sunColor.rgb);
    color += occlusion * pbrAmbient(n, v, baseColor.rgb, roughness, metallic,
                                    frame.skyAmbient.rgb, frame.groundAmbient.rgb);
    color += texture(emissiveMap, inUv).rgb * material.emissiveTint.rgb * material.emissiveTint.a;
    outColor = vec4(applyAtmosphere(color, inWorldPosition), baseColor.a);
}
