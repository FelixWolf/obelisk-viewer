#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// The sky, behind everything: Second Life's windlight sky model (as the
// official viewer's skyF.glsl, with Z up), then the moon and sun (textured
// quads, as llvosky.cpp's) and stars. One full-screen triangle; each pixel works out its view ray.

layout(location = 0) in vec2 inUv;

FRAME_FS_BLOCK

#include "atmosphere.glsl"

layout(set = 2, binding = 0) uniform sampler2D cloudNoise;
layout(set = 2, binding = 1) uniform sampler2D sunMap;
layout(set = 2, binding = 2) uniform sampler2D moonMap;

layout(set = 3, binding = 1) uniform SkyFS
{
    mat4 inverseViewProjection; // of the camera's rotation only
    // The sun's and moon's quads: towards the middle (w: drawn), half
    // their width and height, at a distance of 1 (RendererScene.cpp drawSky).
    vec4 sunCenter;
    vec4 sunRight;
    vec4 sunUp;
    vec4 moonCenter;
    vec4 moonRight;
    vec4 moonUp;
};

// Where a view direction meets the plane of a quad of the sky (as the
// official viewer's heavenly bodies, llvosky.cpp): its texture
// coordinates, 0..1 on the quad. Worked out for every pixel (so their
// derivatives, for the texture's level of detail, are right at its edges).
vec2 quadUv(vec3 dir, vec4 center, vec4 right, vec4 up)
{
    const vec3 p = dir / max(dot(dir, center.xyz), 1e-4) - center.xyz;
    return vec2(dot(p, right.xyz) / dot(right.xyz, right.xyz),
                dot(p, up.xyz) / dot(up.xyz, up.xyz)) * 0.5 + 0.5;
}

bool onQuad(vec3 dir, vec4 center, vec2 uv)
{
    return center.w > 0.5 && dot(dir, center.xyz) > 0.0 && all(greaterThanEqual(uv, vec2(0.0))) &&
           all(lessThanEqual(uv, vec2(1.0)));
}

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

// The cloud layer (as the official viewer's cloudsF.glsl, Z up): noise
// texture on the sky dome, a sphere whose center is below the eye, so it
// reaches down to the horizon. Returns the clouds' color (in the sky
// model's units) and how much they cover.
vec4 cloudLayer(vec3 dir)
{
    const float scale = frame.cloudColor.w;
    if(frame.cloudPos2.w < 0.5 || scale < 0.001 || dir.z <= 0.0)
    {
        return vec4(0.0);
    }
    const vec3 lightDir = frame.atmoLight.xyz;
    const float densityMultiplier = frame.atmoParams.x;
    const float maxY = frame.atmoParams.z;
    const float sunMoonGlow = frame.atmoParams.w;
    const float cloudShadow = frame.atmoAmbient.w;

    // Where the view ray meets the dome.
    const float radius = frame.cloudDome.x;
    const float offset = frame.cloudDome.y;
    const float b = -offset * radius * dir.z;
    const float t = -b + sqrt(b * b + radius * radius * (1.0 - offset * offset));
    const vec3 onDome = dir * t;

    // Planar texture coordinates across the dome (north and west), scaled
    // about the middle.
    vec2 uv1 = vec2(-(1.0 + onDome.y / radius) * 0.5, (1.0 - onDome.x / radius) * 0.5);
    uv1 = (uv1 - 0.5) / scale + 0.5;
    // Shifted towards the light: the clouds' shaded side.
    vec2 uv2 = uv1 + vec2(-lightDir.y, -lightDir.x) * 0.0125;
    vec2 uv3 = uv1 * 16.0;
    vec2 uv4 = uv2 * 16.0;

    vec3 rel = onDome + vec3(0.0, 0.0, 50.0);
    float altitudeBlend = clamp((rel.z + 512.0) / maxY, 0.0, 1.0);
    if(rel.z > 0.0)
    {
        rel *= maxY / rel.z;
    }
    else
    {
        altitudeBlend = 0.0;
        rel *= -32000.0 / min(rel.z, -1e-3);
    }
    const vec3 relDir = normalize(rel);

    const vec3 blueDensity = frame.blueDensity.xyz;
    const float hazeDensity = frame.blueDensity.w;
    const vec3 combinedDensity = abs(blueDensity) + vec3(abs(hazeDensity));
    const vec3 blueWeight = blueDensity / combinedDensity;
    const vec3 hazeWeight = hazeDensity / combinedDensity;
    const vec3 lightAtten = (blueDensity + vec3(hazeDensity * 0.25)) * (densityMultiplier * maxY);

    vec3 sunlight = frame.atmoSunlight.xyz *
                    exp(-lightAtten / max(1e-6, max(0.0, relDir.z) + lightDir.z));
    vec3 haze = exp(-combinedDensity * length(rel) * densityMultiplier);

    float hazeGlow = max(1.0 - dot(relDir, lightDir), 0.001) * frame.glow.x;
    hazeGlow = pow(hazeGlow, frame.glow.z) * sunMoonGlow;
    hazeGlow = sunMoonGlow < 1.0 ? 0.0 : hazeGlow + 0.25;

    const vec3 ambient =
        frame.atmoAmbient.xyz + (1.0 - frame.atmoAmbient.xyz) * cloudShadow * 0.5;
    sunlight *= 1.0 - cloudShadow;
    const vec3 belowCloud =
        frame.blueHorizon.xyz * blueWeight * (sunlight + ambient) +
        frame.blueHorizon.w * hazeWeight * (sunlight * hazeGlow + ambient);

    // The clouds' own light: the sun through twice the air above them.
    const vec3 cloudSun = frame.atmoSunlight.xyz * exp(-lightAtten / max(1e-6, lightDir.z * 2.0));
    vec3 sunSide = cloudSun * hazeGlow * frame.cloudColor.xyz;
    vec3 shadeSide = ambient * frame.cloudColor.xyz;
    haze = sqrt(haze); // thinner air below the clouds
    sunSide *= haze;
    shadeSide = shadeSide * haze + belowCloud * (1.0 - haze);

    // Coverage from two scales of noise, roughened by the variance.
    const vec3 pos1 = frame.cloudPos1.xyz;
    const vec3 pos2 = frame.cloudPos2.xyz;
    const float variance = frame.cloudPos1.w * (1.0 - scale * 0.25);
    const vec2 disturbance =
        vec2(texture(cloudNoise, uv1 / 8.0).x, texture(cloudNoise, (uv3 + uv1) / 16.0).x) * variance;
    const vec2 disturbance2 =
        vec2(texture(cloudNoise, (uv1 + uv3) / 4.0).x, texture(cloudNoise, (uv4 + uv2) / 8.0).x) *
        variance;
    uv1 += pos1.xy + disturbance * 0.2;
    uv2 += pos1.xy;
    uv3 += pos2.xy;
    uv4 += pos2.xy;
    const float varied = min(1.0, (disturbance.x * 2.0 + disturbance.y * 2.0 + disturbance2.x +
                                   disturbance2.y) * 4.0);
    const float density = 2.0 * (cloudShadow - 0.25) * (1.0 - varied * varied);

    float cover = (texture(cloudNoise, uv1).x - 0.5) + (texture(cloudNoise, uv3).x - 0.5) * pos2.z;
    cover = min(max(cover + density, 0.0) * 10.0 * pos1.z, 1.0);
    cover = 1.0 - cover * cover;
    cover = clamp((1.0 - cover * cover) * altitudeBlend, 0.0, 1.0);

    // Self shadowing: how much sunlight reaches this side.
    float shade = texture(cloudNoise, uv2).x - 0.5;
    shade = min(max(shade + density, 0.0) * 2.5 * pos1.z, 1.0);
    shade = 1.0 - shade;
    shade = 1.0 - shade * shade;

    const vec3 color = clamp(sunSide * (1.0 - shade) + shadeSide, vec3(0.0), vec3(1.0)) * 2.0;
    return vec4(color, cover);
}

void main()
{
    vec4 far = inverseViewProjection * vec4(inUv.x * 2.0 - 1.0, 1.0 - inUv.y * 2.0, 1.0, 1.0);
    const vec3 dir = normalize(far.xyz / far.w);

    const vec4 clouds = cloudLayer(dir);
    const vec3 sky = mix(atmosphereSky(dir), clouds.rgb, clouds.a);
    vec3 color = srgbToLinear(sky);
    const float brightness = dot(sky, vec3(0.2126, 0.7152, 0.0722));
    const float clear = 1.0 - clouds.a; // the clouds hide what's behind them

    // The moon (moonF.glsl): its texture times its brightness, fading in
    // as it rises; it hides the stars behind it.
    bool moonCovers = false;
    const vec2 moonUv = quadUv(dir, moonCenter, moonRight, moonUp);
    const vec2 moonDx = dFdx(moonUv);
    const vec2 moonDy = dFdy(moonUv);
    const vec2 sunUv = quadUv(dir, sunCenter, sunRight, sunUp);
    const vec2 sunDx = dFdx(sunUv);
    const vec2 sunDy = dFdy(sunUv);
    if(frame.moonDisc.z > -0.05 && onQuad(dir, moonCenter, moonUv))
    {
        vec4 moon = textureGrad(moonMap, moonUv, moonDx, moonDy);
        if(moon.a > 2.0 / 255.0)
        {
            moonCovers = true;
            const float fade =
                frame.moonDisc.z > 0.0 ? clamp(frame.moonDisc.z * frame.moonDisc.z * 4.0, 0.0, 1.0)
                                       : 1.0;
            moon.rgb *= frame.moonInfo.x;
            color = mix(color, moon.rgb, moon.a * fade * clear);
        }
    }
    // The sun (sunDiscF.glsl): only with a texture.
    if(frame.sunDisc.z > -0.05 && onQuad(dir, sunCenter, sunUv))
    {
        const vec4 sun = textureGrad(sunMap, sunUv, sunDx, sunDy);
        color = mix(color, sun.rgb, sun.a * clear);
    }

    if(dir.z > -0.02 && !moonCovers)
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
                color += vec3(fract(h * 7919.0)) * stars * 0.5 * clear;
            }
        }
    }
    // Under the water, looking down or across: nothing but its fog (the
    // sky above shows through the surface).
    if(frame.waterInfo.y > 0.5 && frame.cameraPosition.z < frame.waterInfo.x && dir.z <= 0.0)
    {
        color = srgbToLinear(frame.waterFog.rgb);
    }
    outColor = vec4(color, 1.0);
}
