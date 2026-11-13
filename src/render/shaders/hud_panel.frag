#version 450

// The HUD image on a panel in the scene (hud_panel.vert): back to linear
// light for the HDR view, still premultiplied.

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec3 inWorldPosition;
layout(set = 2, binding = 0) uniform sampler2D hud;
layout(location = 0) out vec4 outColor;

vec3 srgbToLinear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

void main()
{
    const vec4 texel = texture(hud, inUv);
    // Unpremultiply to decode, then premultiply again.
    const vec3 color = texel.a > 0.0 ? srgbToLinear(texel.rgb / texel.a) * texel.a : vec3(0.0);
    outColor = vec4(color, texel.a);
}
