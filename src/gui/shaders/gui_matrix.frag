#version 450

// The color filters (brightness, contrast, invert, grayscale, sepia,
// hue-rotate, saturate): a matrix on the color, premultiplied (so its
// constant column is times alpha), alpha kept.

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform sampler2D image;
layout(set = 3, binding = 0) uniform MatrixFS
{
    mat4 matrix;
};
layout(location = 0) out vec4 outColor;

void main()
{
    const vec4 texel = texture(image, inUv);
    outColor = vec4((matrix * texel).rgb, texel.a);
}
