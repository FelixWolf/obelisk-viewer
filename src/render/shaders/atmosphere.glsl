// Second Life's atmosphere for surfaces and the sky. Include after
// FRAME_FS_BLOCK (it reads `frame`).

// Shaders that draw what can be under the water define WATER_MASK_BINDING
// (a sampler in set 2): the water mask (RendererWater.cpp), 1 where the
// water is in front, 0 where a water exclusion surface is (or no water).
#ifdef WATER_MASK_BINDING
layout(set = 2, binding = WATER_MASK_BINDING) uniform sampler2D waterMask;

// The mask at this fragment (a 1 by 1 white one stands for all water).
float waterMaskHere()
{
    return texelFetch(waterMask, min(ivec2(gl_FragCoord.xy), textureSize(waterMask, 0) - 1), 0).r;
}
#endif

// The sky settings' colors are roughly sRGB; the scene is linear.
vec3 srgbToLinear(vec3 c)
{
    c = max(c, vec3(0.0));
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

// Haze between the camera and a surface (the windlight model, as the
// official viewer's atmosphericsFuncs.glsl, Z up): the surface's color is
// dimmed by `atten`, and the light the air scatters towards us is added.
vec3 applySky(vec3 color, vec3 worldPosition)
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
    // The haze as hazeF.glsl adds it: brighter for skies made for PBR.
    return color * atten.r + srgbToLinear(additive * 2.0) * frame.moonInfo.y;
}

// Water fog between the camera and a surface under the water, for the
// stretch of the view that's under it (waterFogF.glsl
// getWaterFogViewNoClip, in world space: the plane is z = height).
vec3 applyWaterFog(vec3 color, vec3 worldPosition)
{
    const vec3 rel = worldPosition - frame.cameraPosition.xyz;
    const vec3 view = normalize(rel);
    const float above = frame.cameraPosition.z - frame.waterInfo.x; // the camera's height over it
    const float es = -view.z;
    const float e0 = max(-above, 0.0);
    // From where the view enters the water (or the camera, under it).
    const vec3 entry = above > 0.0 && es > 1e-5 ? view * (above / es) : vec3(0.0);
    const float l = max(length(rel - entry), 0.1);
    const float kd = frame.waterInfo.z;
    const float ks = frame.waterInfo.w;
    const float F = 0.98;
    const float t1 = -kd * pow(F, ks * e0);
    float t2 = kd + ks * es;
    t2 = abs(t2) < 1e-5 ? 1e-5 : t2;
    const float t3 = pow(F, t2 * l) - 1.0;
    const float L = pow(clamp(t1 / t2 * t3, 0.0, 1.0), 1.0 / 1.7);
    const float D = pow(F, l * kd);
    return color * D + srgbToLinear(frame.waterFog.rgb) * L;
}

// The air's haze, or the water's fog for what's under the water
// (waterFogF.glsl applySkyAndWaterFog): from above, only where the water's
// in front of it, not behind a water exclusion surface (waterHazeF.glsl).
vec3 applyAtmosphere(vec3 color, vec3 worldPosition)
{
    if(frame.waterInfo.y > 0.5 && worldPosition.z < frame.waterInfo.x)
    {
#ifdef WATER_MASK_BINDING
        if(frame.cameraPosition.z < frame.waterInfo.x || waterMaskHere() > 0.5)
#endif
        {
            return applyWaterFog(color, worldPosition);
        }
    }
    return applySky(color, worldPosition);
}
