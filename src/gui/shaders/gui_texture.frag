#version 450

// Textured RmlUi geometry (images, glyphs): the texture times the vertices'
// colors (premultiplied). The app's images (asset://) may need reading
// differently: sampled as linear light (sRGB textures) to be encoded as the
// page's values are, not yet premultiplied, stored bottom-up.

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;
layout(set = 2, binding = 0) uniform sampler2D image;
layout(set = 3, binding = 0) uniform TextureFS
{
    vec4 mode; // x encode linear to sRGB, y premultiply, z flip v
};
layout(location = 0) out vec4 outColor;

vec3 linearToSrgb(vec3 c)
{
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main()
{
    const vec2 uv = mode.z > 0.5 ? vec2(inUv.x, 1.0 - inUv.y) : inUv;
    vec4 texel = texture(image, uv);
    if(mode.x > 0.5)
    {
        texel.rgb = linearToSrgb(texel.rgb);
    }
    if(mode.y > 0.5)
    {
        texel.rgb *= texel.a;
    }
    outColor = inColor * texel;
}
