#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Water, as the official viewer's (waterV.glsl, waterF.glsl): three layers
// of waves from the water's normal map (one big and slow, two small and
// quicker, drifting along the settings' wave directions); the scene behind
// it (copied from the view so far) seen through it, bent by the waves; the
// reflection (the probes, at the waves' normals) mixed in by a fresnel
// term; and the sun's highlight. Under it the water's fog (atmosphere.glsl)
// does the rest.

layout(location = 0) in vec3 inWorldPosition;

layout(set = 2, binding = 0) uniform sampler2D normalMap;
layout(set = 2, binding = 1) uniform sampler2D nextNormalMap;
layout(set = 2, binding = 2) uniform sampler2D sceneColor;
layout(set = 2, binding = 3) uniform sampler2DArrayShadow shadowMap;

FRAME_FS_BLOCK

layout(set = 3, binding = 1) uniform WaterFS
{
    vec4 waves;     // xy wave 1 direction, zw wave 2
    vec4 params;    // x time, y normal blend, z fresnel scale, w fresnel offset
    vec4 scale;     // xyz normal scale, w refraction scale (above or below)
    vec4 viewInfo;  // x roughness, y 1 / view width, z 1 / view height, w camera under (1)
}
water;

#define WATER_MASK_BINDING 7
#include "atmosphere.glsl"
#include "shadow.glsl"
#define PROBE_RADIANCE_BINDING 4
#define PROBE_IRRADIANCE_BINDING 5
#define BRDF_TABLE_BINDING 6
#define PROBES_BINDING 8
#define CLUSTERS_BINDING 9
#include "clusters.glsl"
#include "probes.glsl"

layout(location = 0) out vec4 outColor;
// Multisampled: the depth, as a color (setSceneTargets); no target otherwise.
layout(location = 1) out float outDepth;

vec3 wave(vec2 uv)
{
    const vec3 a = texture(normalMap, uv).xyz * 2.0 - 1.0;
    const vec3 b = texture(nextNormalMap, uv).xyz * 2.0 - 1.0;
    return mix(a, b, water.params.y);
}

void main()
{
    outDepth = gl_FragCoord.z;
    const float time = water.params.x;
    const vec3 rel = inWorldPosition - frame.cameraPosition.xyz;
    const float dist = length(rel);
    const vec3 viewVec = rel / max(dist, 1e-4); // from the camera

    // The waves (waterV.glsl): wobbled positions so the tiling hides.
    vec2 v = inWorldPosition.xy;
    v.x += (cos(v.x * 0.08) + sin(v.y * 0.02)) * 6.0;
    const vec2 bigWave = v * 0.04 + water.waves.xy * time * 0.055;
    const vec2 little1 = v * vec2(0.45, 0.9) + water.waves.zw * time * 0.13;
    const vec2 little2 = v * vec2(0.1, 0.2) + water.waves.xy * time * 0.1;
    const vec3 wave1 = wave(bigWave);
    const vec3 wave2 = wave(little1);
    const vec3 wave3 = wave(little2);
    vec3 wavef = (wave1 + wave2 * 0.4 + wave3 * 0.6) * 0.5;

    // How much it reflects: more at grazing angles.
    const float fs = water.params.z;
    const float fo = water.params.w;
    vec3 df3 = max(vec3(0.0), vec3(dot(viewVec, wave1), dot(viewVec, (wave2 + wave3) * 0.5),
                                   dot(viewVec, wave3)) * fs + fo);
    df3 *= df3;
    const vec2 df2 = max(vec2(0.0), vec2(df3.x + df3.y + df3.z, dot(viewVec, wavef) * fs + fo));

    // What's behind it, bent by the waves (from above less far off; from
    // below as underWaterF.glsl, by their sum).
    const bool under = water.viewInfo.w > 0.5;
    const vec2 screen = gl_FragCoord.xy * water.viewInfo.yz;
    // Behind a water exclusion surface (from above): no water, what's
    // behind as it is (waterF.glsl's fade).
    if(!under && waterMaskHere() < 0.5)
    {
        outColor = vec4(texture(sceneColor, screen).rgb, 1.0);
        return;
    }
    const vec2 offset = under ? normalize(wave1 + wave2 + wave3).xy * water.scale.w
                              : wavef.xy * 3.0 * water.scale.w / max(sqrt(dist), 1.0) * 2.0;
    const vec2 bent = clamp(screen + offset, vec2(0.0), vec2(0.999));
    const vec3 behind = texture(sceneColor, bent).rgb;

    const vec3 up = vec3(0.0, 0.0, under ? -1.0 : 1.0);
    vec3 waveIbl = wavef * water.scale.xyz;
    waveIbl.z *= 2.0;
    waveIbl = normalize(waveIbl);
    const vec3 n = normalize(wavef);
    const float vdu = clamp(-dot(viewVec, up) * 2.0, 0.0, 1.0);

    vec3 color = behind;
    if(!under)
    {
        // The reflection: the probes along the waves' reflected view.
        const float roughness = water.viewInfo.x;
        const ProbeLight light = sampleProbes(inWorldPosition, waveIbl, reflect(viewVec, waveIbl),
                                              roughness, false, frame.skyAmbient.rgb);
        const vec3 radiance = light.radiance * df2.y;
        // The sun's highlight (flatter far away, as the official).
        const vec3 hn = normalize(n + up * max(dist, 32.0) / 32.0 * (1.0 - vdu));
        const vec3 l = frame.sunDirection.xyz;
        const vec3 toCamera = -viewVec;
        const float nl = max(dot(hn, l), 0.0);
        const float shadow = sunShadow(shadowMap, inWorldPosition, up);
        // In classic mode the sun's sRGB color, without its 1.35 (waterF.glsl).
        const vec3 sunlight =
            classicMode() ? frame.sunColor.rgb / 1.35 * PI : frame.sunColor.rgb;
        const vec3 punctual =
            clamp(nl * ggxSpecular(hn, toCamera, l, vec3(1.0), max(roughness, 0.05)), vec3(0.0),
                  vec3(10.0)) *
            sunlight * shadow;
        color = mix(behind, radiance, min(1.0, df2.x)) + punctual;
    }
    // From under it, the water's fog up to the surface; from above, the
    // air's haze.
    outColor = vec4(under ? applyWaterFog(color, inWorldPosition)
                          : applySky(color, inWorldPosition),
                    1.0);
}
