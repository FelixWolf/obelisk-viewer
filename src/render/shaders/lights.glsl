// Local lights (Scene::lights, chosen per frame by the renderer). Define
// LIGHTS_BINDING (the storage buffer's binding in set 2) and
// PROJECTORS_BINDING (the projector textures' sampler), and include after
// FRAME_FS_BLOCK.

struct LocalLight
{
    vec4 positionRange;   // xyz position, w range
    vec4 colorFalloff;    // rgb linear color, a falloff
    mat4 projector;       // scene -> projector texture (projectors only)
    vec4 originLayer;     // xyz the projection's apex; w texture layer (-1: white, < -1.5: point light)
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

vec4 projectorTexel(float layer, vec2 uv, float lod)
{
    if(layer < 0.0)
    {
        return vec4(1.0);
    }
    return textureLod(projectorMaps, vec3(uv, layer), lod);
}

// A projector's light, as the official viewer's legacy (non-PBR) deferred
// spot light (spotLightF.glsl): the texture through the frustum, blurring
// (higher mip levels) past the focus distance and fading at its edges, plus
// ambiance, a fill from the blurred texture that reaches what the direct
// light leaves dark.
vec3 projectorLight(LocalLight light, vec3 position, vec3 normal, float attenuation)
{
    vec4 tc = light.projector * vec4(position, 1.0);
    if(tc.z < 0.0)
    {
        return vec3(0.0); // behind the near plane
    }
    tc.xyz /= tc.w;
    if(tc.x <= 0.0 || tc.x >= 1.0 || tc.y <= 0.0 || tc.y >= 1.0 || tc.z <= 0.0)
    {
        return vec3(0.0);
    }
    const float layer = light.originLayer.w;
    const float depth = light.projectorParams.x;
    const float maxLod = light.projectorParams.y;
    const float ambiance = light.projectorParams.z;
    const float lodBias = light.projectorParams.w;

    const vec3 toApex = light.originLayer.xyz - position;
    const float nl = clamp(dot(normal, normalize(toApex)), 1e-6, 1.0);
    const float along = dot(position - light.positionRange.xyz, light.axisFocus.xyz);

    // Direct: sharp at the focus, blurrier beyond; its edges fade over a
    // margin that grows with the blur.
    const float lod = clamp((along - light.axisFocus.w) / depth, 0.0, 1.0) * maxLod;
    vec4 direct = projectorTexel(layer, tc.xy, lod - lodBias);
    const vec2 edges = vec2(0.5) - abs(tc.xy - vec2(0.5));
    const float edge = max(0.25 * min(lod / max(maxLod * 0.5, 1e-4), 1.0), 1e-4);
    direct *= clamp(min(edges.x, edges.y) / edge, 0.0, 1.0);
    const float lit = nl * attenuation;
    vec3 sum = light.colorFalloff.rgb * direct.rgb * direct.a * lit;

    // Ambiance: the most blurred texture, faded towards a circle's edge.
    // Lights what faces away from the projector (as the official viewer).
    vec4 fill = projectorTexel(layer, tc.xy, maxLod - lodBias);
    const vec2 fromCenter = tc.xy - vec2(0.5);
    fill *= clamp((0.25 - dot(fromCenter, fromCenter)) / 0.25, 0.0, 1.0);
    float amount = (nl * 0.5 + 0.5) * ambiance + ambiance + (nl * nl * 0.5 + 0.5) * ambiance;
    amount = min(amount * attenuation, 1.0 - lit);
    const float facing = max(dot(-normalize(toApex), normal), 0.0);
    sum += amount * light.colorFalloff.rgb * fill.rgb * fill.a * facing;
    return sum;
}

// The light reaching a surface from every local light, times N.L (as the
// official viewer's legacy lighting: color x attenuation x N.L).
vec3 localLight(vec3 position, vec3 normal)
{
    vec3 sum = vec3(0.0);
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
        if(light.originLayer.w >= -1.5)
        {
            sum += projectorLight(light, position, normal, attenuation);
            continue;
        }
        const float nl = dot(normal, toLight / max(distance, 1e-4));
        if(nl > 0.0)
        {
            sum += light.colorFalloff.rgb * attenuation * nl;
        }
    }
    return sum;
}
