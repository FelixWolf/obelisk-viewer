#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// The sky, behind everything: Second Life's windlight sky model (as the
// official viewer's skyF.glsl, with Z up), then the sun and moon discs and
// stars. One full-screen triangle; each pixel works out its view ray.

layout(location = 0) in vec2 inUv;

FRAME_FS_BLOCK

#include "atmosphere.glsl"

layout(set = 3, binding = 1) uniform SkyFS
{
    mat4 inverseViewProjection;
};

layout(location = 0) out vec4 outColor;

float hash(vec3 p)
{
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

vec3 atmosphereSky(vec3 dir)
{
    const vec3 lightDir = frame.atmoLight.xyz;
    const float densityMultiplier = frame.atmoParams.x;
    const float maxY = frame.atmoParams.z;
    const float sunMoonGlow = frame.atmoParams.w;

    // A point on the sky dome, a little above the eye; then out to max_y
    // (or far below the horizon).
    vec3 rel = dir * 2000.0 + vec3(0.0, 0.0, 50.0);
    if(rel.z > 0.0)
    {
        rel *= maxY / rel.z;
    }
    else if(rel.z < 0.0)
    {
        rel *= -32000.0 / rel.z;
    }
    const vec3 relDir = normalize(rel);
    const float relLength = length(rel);

    const vec3 blueDensity = frame.blueDensity.xyz;
    const float hazeDensity = frame.blueDensity.w;
    const vec3 blueHorizon = frame.blueHorizon.xyz;
    const float hazeHorizon = frame.blueHorizon.w;
    const vec3 combined = max(abs(blueDensity) + vec3(abs(hazeDensity)), vec3(1e-6));
    const vec3 blueWeight = blueDensity / combined;
    const vec3 hazeWeight = hazeDensity / combined;

    // The moon's light is a dimmer version of the sun's.
    vec3 sunlight = frame.atmoSunlight.w > 0.5 ? frame.atmoSunlight.xyz
                                               : frame.atmoSunlight.xyz * 0.7;
    const vec3 lightAtten = (blueDensity + vec3(hazeDensity * 0.25)) * (densityMultiplier * maxY);
    // Long rays: the light's path depends on both the light and where we look.
    const float offAxis = 1.0 / max(1e-6, max(0.0, relDir.z) + lightDir.z);
    sunlight *= exp(-lightAtten * offAxis);

    vec3 transparency = exp(-combined * relLength * densityMultiplier);

    float hazeGlow = max(1.0 - dot(relDir, lightDir), 0.001) * frame.glow.x;
    hazeGlow = pow(hazeGlow, frame.glow.z);
    hazeGlow = sunMoonGlow < 1.0 ? 0.0 : sunMoonGlow * (hazeGlow + 0.25);

    const vec3 ambient = frame.atmoAmbient.xyz;
    const float cloudShadow = frame.atmoAmbient.w;

    // Above the clouds, then blended towards the darker air below them near
    // the horizon.
    vec3 color = (blueHorizon * blueWeight * (sunlight + ambient) +
                  hazeHorizon * hazeWeight * (sunlight * hazeGlow + ambient)) *
                 (1.0 - transparency);
    const vec3 cloudyAmbient = ambient + max(vec3(0.0), 1.0 - ambient) * cloudShadow * 0.5;
    const vec3 shaded = sunlight * max(0.0, 1.0 - cloudShadow);
    const vec3 below = blueHorizon * blueWeight * (shaded + cloudyAmbient) +
                       hazeHorizon * hazeWeight * (shaded * hazeGlow + cloudyAmbient);
    transparency = sqrt(transparency);
    color += (below - color) * (1.0 - sqrt(transparency));

    return clamp(color * 2.0, vec3(0.0), vec3(5.0));
}

void main()
{
    vec4 far = inverseViewProjection * vec4(inUv.x * 2.0 - 1.0, 1.0 - inUv.y * 2.0, 1.0, 1.0);
    const vec3 dir = normalize(far.xyz / far.w - frame.cameraPosition.xyz);

    const vec3 sky = atmosphereSky(dir);
    vec3 color = srgbToLinear(sky);
    const float brightness = dot(sky, vec3(0.2126, 0.7152, 0.0722));

    if(dir.z > -0.02)
    {
        // Stars: hashed points on a fine grid of directions, fading out as
        // the sky brightens.
        const float stars = frame.glow.w / 250.0 * clamp(1.0 - brightness * 4.0, 0.0, 1.0);
        if(stars > 0.0)
        {
            const vec3 cell = floor(dir * 400.0);
            const float h = hash(cell);
            if(h > 0.9985)
            {
                color += vec3(fract(h * 7919.0)) * stars * 0.5;
            }
        }

        // Sun and moon discs, soft at the edge.
        const float sunCos = dot(dir, frame.sunDisc.xyz);
        const float sunEdge = cos(frame.sunDisc.w);
        if(frame.sunDisc.z > -0.05 && sunCos > sunEdge)
        {
            const float t = smoothstep(sunEdge, mix(sunEdge, 1.0, 0.3), sunCos);
            const vec3 lightAtten = (frame.blueDensity.xyz + vec3(frame.blueDensity.w * 0.25)) *
                                    (frame.atmoParams.x * frame.atmoParams.z);
            const vec3 sunColor = frame.atmoSunlight.xyz *
                                  exp(-lightAtten / max(0.02, frame.sunDisc.z));
            color += srgbToLinear(sunColor) * 40.0 * t;
        }
        const float moonCos = dot(dir, frame.moonDisc.xyz);
        const float moonEdge = cos(frame.moonDisc.w);
        if(frame.moonDisc.z > -0.05 && moonCos > moonEdge)
        {
            const float t = smoothstep(moonEdge, mix(moonEdge, 1.0, 0.15), moonCos);
            color = mix(color, color + vec3(0.55, 0.56, 0.6) * (0.5 + frame.moonInfo.x), t);
        }
    }
    outColor = vec4(color, 1.0);
}
