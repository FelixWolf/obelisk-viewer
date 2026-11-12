// Second Life's atmosphere for surfaces and the sky. Include after
// FRAME_FS_BLOCK (it reads `frame`).

// The sky settings' colors are roughly sRGB; the scene is linear.
vec3 srgbToLinear(vec3 c)
{
    c = max(c, vec3(0.0));
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

// Haze between the camera and a surface (the windlight model, as the
// official viewer's atmosphericsFuncs.glsl, Z up): the surface's color is
// dimmed by `atten`, and the light the air scatters towards us is added.
vec3 applyAtmosphere(vec3 color, vec3 worldPosition)
{
    if(frame.atmoLight.w < 0.5)
    {
        return color;
    }
    const vec3 lightDir = frame.atmoLight.xyz;
    const float densityMultiplier = frame.atmoParams.x;
    const float distanceMultiplier = frame.atmoParams.y;
    const float maxY = frame.atmoParams.z;
    vec3 rel = worldPosition - frame.cameraPosition.xyz;
    if(abs(rel.z) > maxY)
    {
        rel *= maxY / abs(rel.z); // the air thins out above max_y
    }
    const vec3 dir = normalize(rel);
    const float len = length(rel);

    const vec3 blueDensity = frame.blueDensity.xyz;
    const float hazeDensity = frame.blueDensity.w;
    const vec3 combined = max(blueDensity + vec3(hazeDensity), vec3(1e-6));
    const vec3 blueWeight = blueDensity / combined;
    const vec3 hazeWeight = vec3(hazeDensity) / combined;

    // Sunlight dimmed on its way down (more when the sun is low).
    const vec3 lightAtten = (blueDensity + vec3(hazeDensity * 0.25)) * (densityMultiplier * maxY);
    const vec3 sunlight =
        frame.atmoSunlight.xyz * exp(-lightAtten / max(1e-6, lightDir.z));

    const vec3 atten = exp(-combined * len * densityMultiplier * distanceMultiplier);

    // Brighter haze looking towards the light.
    float towards = dot(dir, lightDir) * max(0.0, dot(lightDir, dir));
    float hazeGlow = max(1.0 - towards, 0.001) * frame.glow.x;
    hazeGlow = clamp(pow(hazeGlow, frame.glow.z), -100000.0, 100000.0);
    hazeGlow = (hazeGlow + 0.25) * frame.atmoParams.w;

    const vec3 ambient = frame.atmoAmbient.xyz;
    const float cloudShadow = frame.atmoAmbient.w;
    const vec3 cloudyAmbient = ambient + (vec3(1.0) - ambient) * cloudShadow * 0.5;
    const vec3 shaded = sunlight * (1.0 - cloudShadow);
    vec3 additive = frame.blueHorizon.xyz * blueWeight * (shaded + cloudyAmbient) +
                    frame.blueHorizon.w * hazeWeight * (shaded * hazeGlow + cloudyAmbient);
    additive = min(additive * (vec3(1.0) - atten), vec3(10.0));
    return color * atten.r + srgbToLinear(additive * 2.0);
}
