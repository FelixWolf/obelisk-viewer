// Local lights (Scene::lights, chosen per frame by the renderer). Define
// LIGHTS_BINDING (the storage buffer's binding in set 2) and include after
// FRAME_FS_BLOCK.

struct LocalLight
{
    vec4 positionRange;  // xyz position, w range
    vec4 colorFalloff;   // rgb linear color, a falloff
    vec4 directionCone;  // xyz spot direction, w cosine of the half angle (< -1: point light)
};

layout(std430, set = 2, binding = LIGHTS_BINDING) readonly buffer Lights
{
    LocalLight lights[];
};

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
        const vec3 l = toLight / max(distance, 1e-4);
        const float nl = dot(normal, l);
        if(nl <= 0.0)
        {
            continue;
        }
        const float falloff = light.colorFalloff.a;
        float attenuation = 1.0 - clamp((distance / range + falloff) / (1.0 + falloff), 0.0, 1.0);
        attenuation = attenuation * attenuation * 2.0;
        const float cone = light.directionCone.w;
        if(cone >= -1.0)
        {
            // Soft edge over the outer tenth of the cone.
            const float along = dot(-l, light.directionCone.xyz);
            attenuation *= smoothstep(cone, mix(cone, 1.0, 0.1), along);
        }
        sum += light.colorFalloff.rgb * attenuation * nl;
    }
    return sum;
}
