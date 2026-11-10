// Shared by every shader. SDL GPU's SPIR-V resource layout:
//   vertex:   uniform buffers in set 1
//   fragment: samplers in set 2, uniform buffers in set 3
// Uniform blocks use std140 and must match the C++ structs in Renderer.cpp.

const float PI = 3.14159265359;

// Per-frame lighting, fragment uniform slot 0.
#define FRAME_FS_BLOCK                                                                        \
    layout(set = 3, binding = 0) uniform FrameFS                                              \
    {                                                                                         \
        vec4 cameraPosition;                                                                  \
        vec4 sunDirection; /* towards the sun, normalized */                                  \
        vec4 sunColor;     /* linear RGB * intensity */                                       \
        vec4 skyAmbient;                                                                      \
        vec4 groundAmbient;                                                                   \
    }                                                                                         \
    frame;

// Normal mapping in MikkTSpace: per-pixel bitangent from the interpolated
// (unnormalized) normal and tangent, as the baker computed it.
vec3 perturbNormal(vec3 vertexNormal, vec4 vertexTangent, vec3 tangentSpaceNormal)
{
    vec3 bitangent = vertexTangent.w * cross(vertexNormal, vertexTangent.xyz);
    return normalize(tangentSpaceNormal.x * vertexTangent.xyz +
                     tangentSpaceNormal.y * bitangent + tangentSpaceNormal.z * vertexNormal);
}

vec3 decodeNormal(vec4 texel)
{
    return texel.xyz * 2.0 - 1.0;
}

// Sky above, ground below.
vec3 hemisphereAmbient(vec3 n, vec3 sky, vec3 ground)
{
    return mix(ground, sky, n.z * 0.5 + 0.5);
}

// --- Blinn-Phong ---------------------------------------------------------------

// Energy-normalized so glossy highlights don't get brighter than matte ones.
vec3 blinnPhong(vec3 n, vec3 v, vec3 l, vec3 albedo, vec3 specularColor, float exponent,
                vec3 lightColor)
{
    float nDotL = max(dot(n, l), 0.0);
    vec3 h = normalize(l + v);
    float spec = pow(max(dot(n, h), 0.0), exponent) * (exponent + 8.0) / (8.0 * PI);
    return (albedo / PI + specularColor * spec) * lightColor * nDotL;
}

// --- PBR (glTF metallic-roughness: GGX, Smith, Schlick) -------------------------

float distributionGgx(float nDotH, float alpha)
{
    float a2 = alpha * alpha;
    float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float visibilitySmithGgx(float nDotL, float nDotV, float alpha)
{
    float a2 = alpha * alpha;
    float ggxV = nDotL * sqrt(nDotV * nDotV * (1.0 - a2) + a2);
    float ggxL = nDotV * sqrt(nDotL * nDotL * (1.0 - a2) + a2);
    float sum = ggxV + ggxL;
    return sum > 0.0 ? 0.5 / sum : 0.0;
}

vec3 fresnelSchlick(float vDotH, vec3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - vDotH, 5.0);
}

vec3 pbrDirect(vec3 n, vec3 v, vec3 l, vec3 baseColor, float roughness, float metallic,
               vec3 lightColor)
{
    float nDotL = max(dot(n, l), 0.0);
    if(nDotL <= 0.0)
    {
        return vec3(0.0);
    }
    vec3 h = normalize(l + v);
    float nDotV = max(dot(n, v), 1e-4);
    float nDotH = max(dot(n, h), 0.0);
    float vDotH = max(dot(v, h), 0.0);
    float alpha = max(roughness * roughness, 0.002);

    vec3 f0 = mix(vec3(0.04), baseColor, metallic);
    vec3 f = fresnelSchlick(vDotH, f0);
    vec3 specular = f * distributionGgx(nDotH, alpha) * visibilitySmithGgx(nDotL, nDotV, alpha);
    vec3 diffuse = (1.0 - f) * (1.0 - metallic) * baseColor / PI;
    return (diffuse + specular) * lightColor * nDotL;
}

// Ambient for PBR until there are environment maps: hemisphere light, with
// metals taking a rough "reflection" from the same hemisphere.
vec3 pbrAmbient(vec3 n, vec3 v, vec3 baseColor, float roughness, float metallic, vec3 sky,
                vec3 ground)
{
    vec3 diffuse = hemisphereAmbient(n, sky, ground) * baseColor * (1.0 - metallic);
    vec3 r = reflect(-v, n);
    vec3 f0 = mix(vec3(0.04), baseColor, metallic);
    vec3 env = hemisphereAmbient(normalize(mix(r, n, roughness * roughness)), sky, ground);
    float nDotV = max(dot(n, v), 0.0);
    vec3 fresnel = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - nDotV, 5.0);
    return diffuse + env * fresnel;
}
