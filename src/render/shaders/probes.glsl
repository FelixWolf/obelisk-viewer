// Reflection probes (Renderer::updateProbes): a cube of the surroundings,
// prefiltered so blurrier mip levels stand for rougher surfaces (radiance),
// its cosine-weighted average all around (irradiance), and the split-sum
// table for glossy reflections. As the official viewer's
// reflectionProbeF.glsl, with only its default probe (sky and terrain around
// the camera) so far.
//
// Define PROBE_RADIANCE_BINDING, PROBE_IRRADIANCE_BINDING and
// BRDF_TABLE_BINDING (sampler bindings in set 2) and include after
// FRAME_FS_BLOCK and common.glsl.

layout(set = 2, binding = PROBE_RADIANCE_BINDING) uniform samplerCubeArray probeRadiance;
layout(set = 2, binding = PROBE_IRRADIANCE_BINDING) uniform samplerCubeArray probeIrradiance;
layout(set = 2, binding = BRDF_TABLE_BINDING) uniform sampler2D brdfTable;

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

// Ambient light from all around. Skies made for PBR take it from the probe
// (by the sky's probe ambiance, the rest `amblit`); older skies ("classic
// mode") keep the sky's ambient.
vec3 probeAmbient(vec3 n, vec3 amblit)
{
    if(!probesReady() || frame.probeInfo.w > 0.5)
    {
        return amblit;
    }
    const float ambiance = frame.probeInfo.x;
    const vec3 irradiance =
        textureLod(probeIrradiance, vec4(probeLookup(n), 0.0), 0.0).rgb * ambiance;
    return mix(amblit, irradiance, min(ambiance, 1.0));
}

// What a surface reflects along `direction`: sharp for roughness 0, the
// probe's blurriest level for 1.
vec3 probeReflection(vec3 direction, float roughness)
{
    return textureLod(probeRadiance, vec4(probeLookup(direction), 0.0),
                      clamp(roughness, 0.0, 1.0) * frame.probeInfo.y)
        .rgb;
}

// glTF image-based light: diffuse from the irradiance, specular from the
// radiance at the surface's roughness, scaled and biased by the split-sum
// table. Before the first probe is ready, the hemisphere stand-in.
vec3 pbrProbeLight(vec3 n, vec3 v, vec3 baseColor, float roughness, float metallic, vec3 sky,
                   vec3 ground)
{
    if(!probesReady())
    {
        return pbrAmbient(n, v, baseColor, roughness, metallic, sky, ground);
    }
    const vec3 diffuseColor = baseColor * 0.96 * (1.0 - metallic);
    const vec3 specularColor = mix(vec3(0.04), baseColor, metallic);
    const float nDotV = clamp(abs(dot(n, v)), 0.001, 1.0);
    const vec2 brdf = textureLod(brdfTable, vec2(nDotV, roughness), 0.0).rg;
    const vec3 irradiance = probeAmbient(n, hemisphereAmbient(n, sky, ground));
    const vec3 radiance = probeReflection(reflect(-v, n), roughness);
    return irradiance * diffuseColor + radiance * (specularColor * brdf.x + brdf.y);
}

// Legacy glossy faces: a soft reflection at their glossiness, stronger at
// grazing angles and only on what isn't already bright (the official
// viewer's applyGlossEnv).
void applyGlossyReflection(inout vec3 color, vec3 n, vec3 v, vec3 specularColor,
                           float glossiness)
{
    if(!probesReady())
    {
        return;
    }
    vec3 reflected = min(probeReflection(reflect(-v, n), 1.0 - glossiness), vec3(10.0)) * 0.5;
    float fresnel = clamp(1.0 - dot(v, n), 0.3, 1.0);
    fresnel = fresnel * fresnel * glossiness;
    reflected *= specularColor * fresnel * max(vec3(1.0) - color, vec3(0.0));
    color += reflected * 0.5;
}

// Legacy "shiny" (environment intensity): the sharp reflection, mixed in
// by the intensity (the official viewer's applyLegacyEnv).
void applyShinyReflection(inout vec3 color, vec3 n, vec3 v, float envIntensity)
{
    if(!probesReady())
    {
        return;
    }
    const vec3 reflected = probeReflection(reflect(-v, n), 0.0);
    float fresnel = 1.0 - dot(v, n);
    fresnel = min(fresnel * fresnel + envIntensity, 1.0);
    color = mix(color, reflected * envIntensity * fresnel * 0.5, envIntensity);
}
