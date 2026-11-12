// Shared by every shader. SDL GPU's SPIR-V resource layout:
//   vertex:   storage buffers in set 0, uniform buffers in set 1
//   fragment: samplers then storage buffers in set 2, uniform buffers in set 3
// Uniform blocks use std140 and must match the C++ structs in
// RendererScene.cpp; storage buffers std430.

// One object draw's data (RendererScene.cpp DrawData): every draw's is in
// one storage buffer, and a draw finds its own through an instance-rate
// vertex attribute (the draw's index, as its first instance).
struct DrawData
{
    mat4 model;
    mat4 normalMatrix; // inverse transpose of model (upper 3x3 used)
    vec4 uvRow0;       // texture transform: u' = dot(uvRow0.xyz, (u, v, 1))
    vec4 uvRow1;       //                    v' = dot(uvRow1.xyz, (u, v, 1))
    vec4 material[3];  // the material's values (see the fragment shaders)
    uvec4 skin;        // x: first joint matrix (skinned draws)
};

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
        /* The atmosphere (RendererScene.cpp FrameFS, render::Atmosphere). */                 \
        vec4 atmoLight;    /* xyz towards the sun or moon, w enabled (1) */                   \
        vec4 atmoSunlight; /* xyz light color (settings units), w sun up (1) */               \
        vec4 atmoAmbient;  /* xyz ambient, w cloud shadow */                                  \
        vec4 blueHorizon;  /* xyz, w haze horizon */                                          \
        vec4 blueDensity;  /* xyz, w haze density */                                          \
        vec4 atmoParams;   /* x density mult, y distance mult, z max y, w sun/moon glow */    \
        vec4 glow;         /* xyz glow, w star brightness */                                  \
        vec4 sunDisc;      /* xyz towards the sun, w its radius (radians) */                  \
        vec4 moonDisc;     /* xyz towards the moon, w its radius */                           \
        vec4 moonInfo;     /* x moon brightness */                                            \
        vec4 cloudColor;   /* xyz, w scale */                                                 \
        vec4 cloudPos1;    /* xyz position/density 1, w variance */                           \
        vec4 cloudPos2;    /* xyz position/density 2, w clouds on (1) */                      \
        vec4 cloudDome;    /* x radius, y offset */                                           \
        /* Shadows (shadow.glsl). */                                                          \
        mat4 shadowMatrices[4]; /* world -> each cascade's light clip space */                \
        vec4 shadowSplits;      /* each cascade's far end, as depth along the view */         \
        vec4 shadowTexels;      /* each cascade's texel size, meters */                       \
        vec4 shadowInfo;        /* x on (1), y one texel in texture coordinates */            \
        vec4 viewAxis;          /* xyz the camera's forward direction */                      \
        vec4 lightInfo;         /* x local lights in use (lights.glsl) */                    \
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

// --- Terrain layer blending ---------------------------------------------------

// Weights of the four terrain layers. The layer value picks the pair of
// layers; the blend ramp (indexed by the fractional position and the blend
// noise) shapes the transition, as in the official viewer's terrain shaders.
vec4 terrainLayerWeights(sampler2D ramp, vec2 blend)
{
    float value = blend.x;
    float alpha1 = texture(ramp, vec2(value, blend.y)).r;       // layer 0 vs 1
    float alphaFinal = texture(ramp, vec2(value - 1.0, blend.y)).r; // 0/1 vs 2/3
    float alpha2 = texture(ramp, vec2(value - 2.0, blend.y)).r; // layer 2 vs 3
    return vec4(alphaFinal * alpha1, alphaFinal * (1.0 - alpha1), (1.0 - alphaFinal) * alpha2,
                (1.0 - alphaFinal) * (1.0 - alpha2));
}
