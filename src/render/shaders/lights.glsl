// Local lights (Scene::lights, chosen per frame by the renderer). Define
// LIGHTS_BINDING (the storage buffer's binding in set 2) and
// PROJECTORS_BINDING (the projector textures' sampler), and include after
// FRAME_FS_BLOCK.

struct LocalLight
{
    vec4 positionRange;   // xyz position, w range
    vec4 colorFalloff;    // rgb linear color, a falloff
    mat4 projector;       // scene -> projector texture (projectors only)
    vec4 originLayer;     // xyz the projection's apex; w texture layer (-1: not in yet, < -1.5: point light)
    vec4 axisFocus;       // xyz the way it shines, w focus distance
    vec4 projectorParams; // x near to far plane, y the texture's mip levels (log2 width), z ambiance,
                          // w how many more its own has than the array's
};

layout(std430, set = 2, binding = LIGHTS_BINDING) readonly buffer Lights
{
    LocalLight lights[];
};

layout(set = 2, binding = PROJECTORS_BINDING) uniform sampler2DArray projectorMaps;

// llrender's legacy distance attenuation (deferredUtil.glsl).
float legacyAttenuation(float distance, float range, float falloff)
{
    float a = 1.0 - clamp((distance / range + falloff) / (1.0 + falloff), 0.0, 1.0);
    return a * a * 2.0;
}

// A projector whose texture isn't in yet shines the official viewer's
// default image (llviewertexture.cpp: flat 0x7f gray; this is it linear),
// not white: much dimmer, as it should be until the real one arrives.
const float kProjectorPlaceholder = 0.2122;

vec4 projectorTexel(float layer, vec2 uv, float lod)
{
    if(layer < 0.0)
    {
        return vec4(vec3(kProjectorPlaceholder), 1.0);
    }
    return textureLod(projectorMaps, vec3(uv, layer), lod);
}

// What one light gives a surface: its diffuse light (times N.L and
// attenuation already: multiply by the surface color), the light arriving
// (attenuated, not times N.L: for highlights) and where from.
struct LightSample
{
    vec3 diffuse;
    vec3 incoming;
    vec3 toLight; // unit length
};

// A projector's light, as the official viewer's deferred spot light
// (spotLightF.glsl): the texture through the frustum, blurring (higher mip
// levels) past the focus distance and fading at its edges, plus ambiance, a
// fill from the blurred texture that reaches what the direct light leaves
// dark (diffuse only).
LightSample projectorLight(LocalLight light, vec3 position, vec3 normal, float attenuation)
{
    LightSample result = LightSample(vec3(0.0), vec3(0.0), vec3(0.0, 0.0, 1.0));
    vec4 tc = light.projector * vec4(position, 1.0);
    if(tc.z < 0.0)
    {
        return result; // behind the near plane
    }
    tc.xyz /= tc.w;
    if(tc.x <= 0.0 || tc.x >= 1.0 || tc.y <= 0.0 || tc.y >= 1.0 || tc.z <= 0.0)
    {
        return result;
    }
    const float layer = light.originLayer.w;
    const float depth = light.projectorParams.x;
    const float maxLod = light.projectorParams.y;
    const float ambiance = light.projectorParams.z;
    const float lodBias = light.projectorParams.w;

    const vec3 toApex = light.originLayer.xyz - position;
    result.toLight = normalize(toApex);
    const float facingNl = dot(normal, result.toLight);
    const float nl = clamp(facingNl, 1e-6, 1.0);
    const float along = dot(position - light.positionRange.xyz, light.axisFocus.xyz);

    // Direct: sharp at the focus, blurrier beyond; its edges fade over a
    // margin that grows with the blur.
    const float lod = clamp((along - light.axisFocus.w) / depth, 0.0, 1.0) * maxLod;
    vec4 direct = projectorTexel(layer, tc.xy, lod - lodBias);
    const vec2 edges = vec2(0.5) - abs(tc.xy - vec2(0.5));
    const float edge = max(0.25 * min(lod / max(maxLod * 0.5, 1e-4), 1.0), 1e-4);
    direct *= clamp(min(edges.x, edges.y) / edge, 0.0, 1.0);
    const float lit = nl * attenuation;
    result.incoming = light.colorFalloff.rgb * direct.rgb * direct.a * attenuation;
    result.diffuse = result.incoming * nl;

    // Ambiance: the most blurred texture, faded towards a circle's edge.
    // Lights what faces away from the projector (as the official viewer).
    vec4 fill = projectorTexel(layer, tc.xy, maxLod - lodBias);
    const vec2 fromCenter = tc.xy - vec2(0.5);
    fill *= clamp((0.25 - dot(fromCenter, fromCenter)) / 0.25, 0.0, 1.0);
    float amount = ambiance + (facingNl * facingNl * 0.5 + 0.5) * ambiance;
    if(facingNl > 0.0)
    {
        amount += (facingNl * 0.5 + 0.5) * ambiance;
    }
    amount = min(amount * attenuation, 1.0 - lit);
    const float facing = max(dot(-result.toLight, normal), 0.0);
    result.diffuse += amount * light.colorFalloff.rgb * fill.rgb * fill.a * facing;
    return result;
}

// Light from every local light: diffuse (color x attenuation x N.L, as the
// official viewer's legacy lighting; multiply by the surface color) and
// highlights, as the official point and spot light shaders:
// - legacy faces (pbr false): the legacy highlight at `glossiness`
//   (legacySpecular; multiply by the specular color), a projector's at
//   most 1;
// - PBR (pbr true): GGX with Fresnel reflectance `f0` at `roughness`, times
//   3.25 (the official "balance with legacy" factor), at most 10.
struct LocalLighting
{
    vec3 diffuse;
    vec3 specular;
};

LocalLighting localLights(vec3 position, vec3 normal, vec3 v, bool pbr, float glossiness, vec3 f0,
                          float roughness)
{
    LocalLighting sum = LocalLighting(vec3(0.0), vec3(0.0));
    const bool highlights = pbr || glossiness > 0.0;
    const int count = int(frame.lightInfo.x);
    for(int i = 0; i < count; i++)
    {
        const LocalLight light = lights[i];
        const vec3 toLight = light.positionRange.xyz - position;
        const float distance = length(toLight);
        const float range = light.positionRange.w;
        if(distance >= range)
        {
            continue;
        }
        const float attenuation = legacyAttenuation(distance, range, light.colorFalloff.a);
        const bool projector = light.originLayer.w >= -1.5;
        LightSample s;
        if(projector)
        {
            s = projectorLight(light, position, normal, attenuation);
        }
        else
        {
            s.toLight = toLight / max(distance, 1e-4);
            s.incoming = light.colorFalloff.rgb * attenuation;
            s.diffuse = s.incoming * max(dot(normal, s.toLight), 0.0);
        }
        sum.diffuse += s.diffuse;
        if(!highlights || dot(normal, s.toLight) <= 0.0)
        {
            continue;
        }
        if(pbr)
        {
            const float nl = dot(normal, s.toLight);
            sum.specular += min(s.incoming * 3.25 * nl * ggxSpecular(normal, v, s.toLight, f0, roughness),
                                vec3(10.0));
        }
        else
        {
            const vec3 highlight = s.incoming * legacySpecular(normal, v, s.toLight, glossiness);
            sum.specular += projector ? min(highlight, vec3(1.0)) : highlight;
        }
    }
    return sum;
}

// Diffuse light only (multiply by the surface color).
vec3 localLight(vec3 position, vec3 normal)
{
    return localLights(position, normal, normal, false, 0.0, vec3(0.0), 1.0).diffuse;
}
