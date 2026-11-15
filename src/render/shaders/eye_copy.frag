#version 450

// An eye's image (sRGB-encoded, as the views' output) into a headset's
// image, pixel for pixel, as linear values: OpenXR takes them as linear (an
// sRGB image encodes them again on the way in).

layout(set = 2, binding = 0) uniform sampler2D eye;
layout(location = 0) out vec4 outColor;

vec3 decode(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main()
{
    const vec4 c = texelFetch(eye, ivec2(gl_FragCoord.xy), 0);
    outColor = vec4(decode(c.rgb), 1.0);
}
