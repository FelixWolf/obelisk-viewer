// Reflection probes (RendererProbes.cpp): cubes of the surroundings,
// prefiltered so blurrier mip levels stand for rougher surfaces (radiance),
// their cosine-weighted average all around (irradiance), and the split-sum
// table for glossy reflections. As the official viewer's
// reflectionProbeF.glsl: a default probe (cube 0: the sky and terrain) and
// local probes (the list below), blended by where the surface is.
//
// Define PROBE_RADIANCE_BINDING, PROBE_IRRADIANCE_BINDING and
// BRDF_TABLE_BINDING (sampler bindings in set 2) and PROBES_BINDING (the
// probe list, a storage buffer in set 2), and include after FRAME_FS_BLOCK
// and common.glsl and clusters.glsl.

layout(set = 2, binding = PROBE_RADIANCE_BINDING) uniform samplerCubeArray probeRadiance;
layout(set = 2, binding = PROBE_IRRADIANCE_BINDING) uniform samplerCubeArray probeIrradiance;
layout(set = 2, binding = BRDF_TABLE_BINDING) uniform sampler2D brdfTable;

// One local probe (RendererProbes.cpp ProbeData), nearest the camera first.
struct ReflectionProbe
{
    vec4 sphere; // xyz origin, w radius
    vec4 params; // x ambiance, y fade in (0..1), z cube, w 0 automatic, 1 manual sphere, 2 manual box
    mat4 box;    // box probes: scene -> the box as a cube from -1 to 1
};
layout(std430, set = 2, binding = PROBES_BINDING) readonly buffer Probes
{
    ReflectionProbe probes[];
};

// The cubes hold the world mirrored in y (so their faces render with the
// same winding as the camera): a world direction is looked up mirrored.
vec3 probeLookup(vec3 direction)
{
    return vec3(direction.x, -direction.y, direction.z);
}

bool probesReady()
{
    return frame.probeInfo.z > 0.5;
}

// Where a ray from inside a sphere leaves it.
vec3 leaveSphere(vec3 origin, vec3 direction, vec3 center, float radius)
{
    const vec3 l = center - origin;
    const float along = dot(l, direction);
    const float d2 = dot(l, l) - along * along;
    return origin + direction * (along + sqrt(max(radius * radius - d2, 0.0)));
}

// Where a ray from inside a box (scene -> unit cube `box`) leaves it, the
// box grown by `scale`.
vec3 leaveBox(vec3 origin, vec3 direction, mat4 box, float scale)
{
    const vec3 rayLocal = mat3(box) * direction;
    const vec3 local = (box * vec4(origin, 1.0)).xyz;
    const vec3 first = (vec3(scale) - local) / rayLocal;
    const vec3 second = (vec3(-scale) - local) / rayLocal;
    const vec3 furthest = max(first, second);
    return origin + direction * min(furthest.x, min(furthest.y, furthest.z));
}

// What a surface gets from the probes around it.
struct ProbeLight
{
    vec3 irradiance; // ambient light (amblit where the sky's is used)
    vec3 radiance;   // reflected, at the surface's roughness
    vec3 sharp;      // reflected, sharp ("shiny" legacy faces)
};

// The light at `position` (normal n, reflection direction r): the local
// probes there weighted as the official viewer weighs them (nearer the
// middle counts more; manual probes over automatic ones, and inside a box
// no automatic ones at all), the default probe where nothing else reaches.
// Skies made before PBR keep the sky's ambient ("classic mode").
ProbeLight sampleProbes(vec3 position, vec3 n, vec3 r, float roughness, bool wantSharp,
                        vec3 amblit)
{
    const float lod = clamp(roughness, 0.0, 1.0) * frame.probeInfo.y;
    const bool classic = frame.probeInfo.w > 0.5;
    const float skyAmbiance = frame.probeInfo.x;
    // [0] automatic, [1] manual.
    vec3 irradiance[2] = vec3[2](vec3(0.0), vec3(0.0));
    vec3 radiance[2] = vec3[2](vec3(0.0), vec3(0.0));
    vec3 sharp[2] = vec3[2](vec3(0.0), vec3(0.0));
    float weights[2] = float[2](0.0, 0.0);
    float distanceWeights[2] = float[2](0.0, 0.0);
    bool inBox = false;

    // Those reaching its cluster (all in use if there are no clusters).
    const int cluster = clusterOf(position);
    const int count = cluster < 0 ? int(frame.lightInfo.y) : int(clusterData[cluster * 4 + 3]);
    const uint first = cluster < 0 ? 0u : clusterData[cluster * 4 + 2];
    for(int k = 0; k < count; k++)
    {
        const int i = cluster < 0 ? k : int(clusterData[first + uint(k)]);
        const ReflectionProbe p = probes[i];
        const int kind = int(p.params.w + 0.5);
        const vec3 center = p.sphere.xyz;
        const float radius = p.sphere.w;
        float w;
        float dw = 0.0;
        vec3 rDirection;
        vec3 nDirection;
        if(kind == 2)
        {
            const vec3 local = (p.box * vec4(position, 1.0)).xyz;
            const float outside = max(abs(local.x), max(abs(local.y), abs(local.z)));
            if(outside > 1.0)
            {
                continue;
            }
            inBox = true;
            w = max(1.0 - outside, 0.001);
            rDirection = leaveBox(position, r, p.box, 1.0) - center;
            nDirection = leaveBox(position, n, p.box, 3.0) - center;
        }
        else
        {
            const float d = max(length(position - center), 0.001);
            if(d > radius)
            {
                continue;
            }
            // Full weight within half the radius, fading to none at it.
            const float inner = radius * 0.5;
            const float fade = 1.0 - max(d - inner, 0.0) / max(radius - inner, 0.001);
            w = p.params.y / d;
            dw = w * fade * max(radius, 1.0) * 4.0;
            w *= fade;
            // Automatic probes: no parallax (the official viewer leaves it
            // out, or the world would show their spheres).
            rDirection = kind == 1 ? leaveSphere(position, r, center, radius) - center : r;
            nDirection = kind == 1 ? leaveSphere(position, n, center, radius) - center : n;
        }
        const float layer = p.params.z;
        const int group = kind == 0 ? 0 : 1;
        const float ambiance = p.params.x;
        vec3 irr = textureLod(probeIrradiance, vec4(probeLookup(nDirection), layer), 0.0).rgb;
        irr = mix(amblit, irr * ambiance, min(ambiance, 1.0));
        irradiance[group] += irr * w;
        radiance[group] +=
            textureLod(probeRadiance, vec4(probeLookup(rDirection), layer), lod).rgb * w;
        if(wantSharp)
        {
            sharp[group] +=
                textureLod(probeRadiance, vec4(probeLookup(rDirection), layer), 0.0).rgb * w;
        }
        weights[group] += w;
        distanceWeights[group] += dw;
    }

    // The default probe smooths over between automatic ones: a small weight,
    // so anything nearer outweighs it.
    if(!inBox && probesReady())
    {
        const float w = 1e-3;
        vec3 irr = textureLod(probeIrradiance, vec4(probeLookup(n), 0.0), 0.0).rgb;
        irradiance[0] += mix(amblit, irr * skyAmbiance, min(skyAmbiance, 1.0)) * w;
        radiance[0] += textureLod(probeRadiance, vec4(probeLookup(r), 0.0), lod).rgb * w;
        if(wantSharp)
        {
            sharp[0] += textureLod(probeRadiance, vec4(probeLookup(r), 0.0), 0.0).rgb * w;
        }
        weights[0] += w;
    }

    ProbeLight light;
    light.irradiance = amblit;
    light.radiance = vec3(0.0);
    light.sharp = vec3(0.0);
    const bool automatic = !inBox && weights[0] > 0.0;
    const bool manual = weights[1] > 0.0;
    if(automatic && manual)
    {
        const float m = min(distanceWeights[1], 1.0);
        light.irradiance = mix(irradiance[0] / weights[0], irradiance[1] / weights[1], m);
        light.radiance = mix(radiance[0] / weights[0], radiance[1] / weights[1], m);
        light.sharp = mix(sharp[0] / weights[0], sharp[1] / weights[1], m);
    }
    else if(automatic)
    {
        light.irradiance = irradiance[0] / weights[0];
        light.radiance = radiance[0] / weights[0];
        light.sharp = sharp[0] / weights[0];
    }
    else if(manual)
    {
        light.irradiance = irradiance[1] / weights[1];
        light.radiance = radiance[1] / weights[1];
        light.sharp = sharp[1] / weights[1];
    }
    if(classic)
    {
        light.irradiance = amblit;
    }
    return light;
}

bool anyProbes()
{
    return probesReady() || frame.lightInfo.y > 0.5;
}

// Ambient light from all around, for surfaces that don't reflect.
vec3 probeAmbient(vec3 position, vec3 n, vec3 amblit)
{
    if(!anyProbes() || frame.probeInfo.w > 0.5)
    {
        return amblit;
    }
    return sampleProbes(position, n, n, 1.0, false, amblit).irradiance;
}

// glTF image-based light: diffuse from the irradiance, specular from the
// radiance at the surface's roughness, scaled and biased by the split-sum
// table. Before any probe is ready, the hemisphere stand-in.
vec3 pbrProbeLight(vec3 position, vec3 n, vec3 v, vec3 baseColor, float roughness,
                   float metallic, vec3 sky, vec3 ground)
{
    if(!anyProbes())
    {
        return pbrAmbient(n, v, baseColor, roughness, metallic, sky, ground);
    }
    const ProbeLight light = sampleProbes(position, n, reflect(-v, n), roughness, false,
                                          hemisphereAmbient(n, sky, ground));
    const vec3 diffuseColor = baseColor * 0.96 * (1.0 - metallic);
    const vec3 specularColor = mix(vec3(0.04), baseColor, metallic);
    const float nDotV = clamp(abs(dot(n, v)), 0.001, 1.0);
    const vec2 brdf = textureLod(brdfTable, vec2(nDotV, roughness), 0.0).rg;
    return light.irradiance * diffuseColor + light.radiance * (specularColor * brdf.x + brdf.y);
}

// Legacy glossy faces: a soft reflection at their glossiness, stronger at
// grazing angles and only on what isn't already bright (the official
// viewer's applyGlossEnv).
void applyGlossyReflection(inout vec3 color, vec3 reflected, vec3 n, vec3 v, vec3 specularColor,
                           float glossiness)
{
    reflected = min(reflected, vec3(10.0)) * 0.5;
    float fresnel = clamp(1.0 - dot(v, n), 0.3, 1.0);
    fresnel = fresnel * fresnel * glossiness;
    reflected *= specularColor * fresnel * max(vec3(1.0) - color, vec3(0.0));
    color += reflected * 0.5;
}

// Legacy "shiny" (environment intensity): the sharp reflection, mixed in
// by the intensity (the official viewer's applyLegacyEnv).
void applyShinyReflection(inout vec3 color, vec3 reflected, vec3 n, vec3 v, float envIntensity)
{
    float fresnel = 1.0 - dot(v, n);
    fresnel = min(fresnel * fresnel + envIntensity, 1.0);
    color = mix(color, reflected * envIntensity * fresnel * 0.5, envIntensity);
}
