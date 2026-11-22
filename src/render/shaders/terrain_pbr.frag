#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// PBR terrain: four PBR materials, each map a 4-layer array, blended by the
// layer weights. Textures are sampled triplanar (projected along X, Y and Z,
// weighted by the surface normal) so steep slopes don't stretch them.
// Thresholds and sharpness match the official viewer's defaults
// (pbrterrainUtilF.glsl, RenderTerrainPBRTriplanarBlendFactor = 8).

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inTexturePosition;
layout(location = 3) in vec2 inBlend;

layout(set = 2, binding = 0) uniform sampler2DArray albedoMaps;
layout(set = 2, binding = 1) uniform sampler2DArray ormMaps;
layout(set = 2, binding = 2) uniform sampler2DArray emissiveMaps;
layout(set = 2, binding = 3) uniform sampler2DArray normalMaps;
layout(set = 2, binding = 4) uniform sampler2D blendRamp;
layout(set = 2, binding = 5) uniform sampler2DArrayShadow shadowMap;

FRAME_FS_BLOCK

#define WATER_MASK_BINDING 10
#include "atmosphere.glsl"
#include "shadow.glsl"
#define PROJECTORS_BINDING 6
#define PROBE_RADIANCE_BINDING 7
#define PROBE_IRRADIANCE_BINDING 8
#define BRDF_TABLE_BINDING 9
#define LIGHTS_BINDING 11
#define PROBES_BINDING 12
#define CLUSTERS_BINDING 13
#include "clusters.glsl"
#include "lights.glsl"
#include "probes.glsl"

layout(set = 3, binding = 1) uniform MaterialFS
{
    vec4 baseColorFactor[4];
    vec4 emissiveTint[4]; // RGB tint, A strength
    vec4 factors[4];      // x roughness, y metallic, z occlusion strength
    // Each layer's texture transform (from region-local meters: rows of a
    // 2x3 matrix), and its linear part's inverse (rows a b, c d), which
    // turns the projection's tangents to follow it.
    vec4 uvRow0[4];
    vec4 uvRow1[4];
    vec4 uvInverse[4];
} material;

layout(location = 0) out vec4 outColor;

const float kLayerThreshold = 0.1;      // drop layers contributing less than this
const float kTriplanarSharpness = 8.0;
const float kTriplanarThreshold = 0.01; // drop projections contributing less than this

struct Sample
{
    vec3 baseColor;
    vec3 orm;
    vec3 emissive;
    vec3 normal; // world space
};

// One projection of one layer. `position` is the projection's plane
// coordinates in meters; `tangent` and `bitangent` the world directions in
// which they increase. The layer's transform applies to both (as the
// official viewer's terrain_texture_transform, the same for every
// projection).
Sample sampleProjection(int layer, vec2 position, vec3 tangent, vec3 bitangent, vec3 n)
{
    const vec3 p = vec3(position, 1.0);
    const vec2 uv = vec2(dot(material.uvRow0[layer].xyz, p), dot(material.uvRow1[layer].xyz, p));
    const vec4 inv = material.uvInverse[layer];
    const vec3 du = inv.x * tangent + inv.z * bitangent;
    const vec3 dv = inv.y * tangent + inv.w * bitangent;
    tangent = du;
    bitangent = dv;
    vec3 uvw = vec3(uv, float(layer));
    Sample s;
    s.baseColor = texture(albedoMaps, uvw).rgb * material.baseColorFactor[layer].rgb;
    vec3 orm = texture(ormMaps, uvw).rgb;
    s.orm = vec3(mix(1.0, orm.r, material.factors[layer].z), orm.g * material.factors[layer].x,
                 orm.b * material.factors[layer].y);
    s.emissive = texture(emissiveMaps, uvw).rgb * material.emissiveTint[layer].rgb *
                 material.emissiveTint[layer].a;
    vec3 t = normalize(tangent - n * dot(n, tangent));
    vec3 b = normalize(bitangent - n * dot(n, bitangent));
    vec3 tn = decodeNormal(texture(normalMaps, uvw));
    s.normal = tn.x * t + tn.y * b + tn.z * n;
    return s;
}

void main()
{
    vec3 n = normalize(inNormal);

    vec4 layerWeights = max(terrainLayerWeights(blendRamp, inBlend) - kLayerThreshold, 0.0);
    layerWeights /= dot(layerWeights, vec4(1.0));

    vec3 planeWeights = pow(abs(n), vec3(kTriplanarSharpness));
    planeWeights /= planeWeights.x + planeWeights.y + planeWeights.z;
    planeWeights = max(planeWeights - kTriplanarThreshold, 0.0);
    planeWeights /= planeWeights.x + planeWeights.y + planeWeights.z;

    // Projections, mirrored on the far sides so textures aren't backwards.
    vec3 p = inTexturePosition;
    float sx = n.x >= 0.0 ? 1.0 : -1.0;
    float sy = n.y >= 0.0 ? 1.0 : -1.0;
    vec2 uvX = vec2(p.y * sx, p.z);
    vec2 uvY = vec2(-p.x * sy, p.z);
    vec2 uvZ = p.xy;
    vec3 tangentX = vec3(0.0, sx, 0.0);
    vec3 tangentY = vec3(-sy, 0.0, 0.0);
    vec3 up = vec3(0.0, 0.0, 1.0);

    vec3 baseColor = vec3(0.0);
    vec3 orm = vec3(0.0);
    vec3 emissive = vec3(0.0);
    vec3 normal = vec3(0.0);
    for(int layer = 0; layer < 4; layer++)
    {
        float lw = layerWeights[layer];
        if(lw <= 0.0)
        {
            continue;
        }
        for(int axis = 0; axis < 3; axis++)
        {
            float w = lw * planeWeights[axis];
            if(w <= 0.0)
            {
                continue;
            }
            Sample s = axis == 0   ? sampleProjection(layer, uvX, tangentX, up, n)
                       : axis == 1 ? sampleProjection(layer, uvY, tangentY, up, n)
                                   : sampleProjection(layer, uvZ, vec3(1.0, 0.0, 0.0),
                                                      vec3(0.0, 1.0, 0.0), n);
            baseColor += s.baseColor * w;
            orm += s.orm * w;
            emissive += s.emissive * w;
            normal += s.normal * w;
        }
    }
    n = normalize(normal);

    float roughness = clamp(orm.g, 0.0, 1.0);
    float metallic = clamp(orm.b, 0.0, 1.0);
    vec3 v = normalize(frame.cameraPosition.xyz - inWorldPosition);
    vec3 color = pbrDirect(n, v, frame.sunDirection.xyz, baseColor, roughness, metallic,
                           frame.sunColor.rgb) *
                 sunShadow(shadowMap, inWorldPosition, normalize(inNormal));
    const float amb = ambientLighting(n, frame.sunDirection.xyz);
    color += orm.r * pbrProbeLight(inWorldPosition, n, v, baseColor, roughness, metallic,
                                   frame.skyAmbient.rgb * amb, frame.groundAmbient.rgb * amb);
    color += baseColor * (1.0 - metallic) * localLight(inWorldPosition, n);
    color += emissive;
    outColor = vec4(applyAtmosphere(color, inWorldPosition), 1.0);
}
