#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Default (legacy) objects: diffuse, normal and specular maps.
//   diffuse:  RGB color, A opacity
//   normal:   RGB tangent-space normal, A glossiness (scales the exponent)
//   specular: RGB specular color, A environment intensity (unused for now)

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUv;

layout(set = 2, binding = 0) uniform sampler2D diffuseMap;
layout(set = 2, binding = 1) uniform sampler2D normalMap;
layout(set = 2, binding = 2) uniform sampler2D specularMap;

FRAME_FS_BLOCK

layout(set = 3, binding = 1) uniform MaterialFS
{
    vec4 diffuseTint;   // RGB color, A opacity
    vec4 specularTint;  // RGB color, A max exponent
    vec4 options;       // x alpha cutoff (< 0: none), y fullbright (unlit) if > 0
} material;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 albedo = texture(diffuseMap, inUv) * material.diffuseTint;
    if(albedo.a < material.options.x)
    {
        discard;
    }
    if(material.options.y > 0.0)
    {
        outColor = albedo;
        return;
    }
    vec4 normalTexel = texture(normalMap, inUv);
    vec3 n = perturbNormal(inNormal, inTangent, decodeNormal(normalTexel));
    if(!gl_FrontFacing)
    {
        n = -n;
    }
    vec3 v = normalize(frame.cameraPosition.xyz - inWorldPosition);
    vec3 l = frame.sunDirection.xyz;

    vec3 specularColor = texture(specularMap, inUv).rgb * material.specularTint.rgb;
    float exponent = max(normalTexel.a * material.specularTint.a, 1.0);

    vec3 color = blinnPhong(n, v, l, albedo.rgb, specularColor, exponent, frame.sunColor.rgb);
    color += albedo.rgb * hemisphereAmbient(n, frame.skyAmbient.rgb, frame.groundAmbient.rgb);
    outColor = vec4(color, albedo.a);
}
