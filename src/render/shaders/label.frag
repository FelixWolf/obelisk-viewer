#version 450

// A glyph: the atlas's coverage times its color, premultiplied (blended
// over sRGB-encoded values, as the official viewer's text is).

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;
layout(set = 2, binding = 0) uniform sampler2D glyphs;
layout(location = 0) out vec4 outColor;

void main()
{
    const float alpha = texture(glyphs, inUv).r * inColor.a;
    if(alpha <= 0.0)
    {
        discard;
    }
    outColor = vec4(inColor.rgb * alpha, alpha);
}
