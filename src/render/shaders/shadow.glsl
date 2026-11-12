// Sun shadows: cascaded shadow maps (see RendererScene.cpp). Include after
// FRAME_FS_BLOCK.

// How much of the sun reaches `worldPosition` (1 lit, 0 shadowed), with
// `normal` the surface's (to push the lookup off the surface: no acne).
float sunShadow(sampler2DArrayShadow shadowMap, vec3 worldPosition, vec3 normal)
{
    if(frame.shadowInfo.x < 0.5)
    {
        return 1.0;
    }
    const float depth = dot(worldPosition - frame.cameraPosition.xyz, frame.viewAxis.xyz);
    const vec4 splits = frame.shadowSplits;
    if(depth > splits.w)
    {
        return 1.0;
    }
    const int cascade = depth < splits.x ? 0 : depth < splits.y ? 1 : depth < splits.z ? 2 : 3;
    const float texel = frame.shadowTexels[cascade];

    // Facing away from the light: the lighting already has it dark.
    const vec3 n = normalize(normal);
    const vec3 p = worldPosition + n * texel * 1.5;
    const vec4 light = frame.shadowMatrices[cascade] * vec4(p, 1.0);
    const vec2 uv = vec2(light.x * 0.5 + 0.5, 0.5 - light.y * 0.5);
    if(any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || light.z > 1.0)
    {
        return 1.0;
    }

    // 3x3 filtered lookups (each one compares and blends four texels).
    const float step = frame.shadowInfo.y;
    float lit = 0.0;
    for(int y = -1; y <= 1; y++)
    {
        for(int x = -1; x <= 1; x++)
        {
            lit += texture(shadowMap,
                           vec4(uv + vec2(x, y) * step, float(cascade), light.z - 0.0005));
        }
    }
    lit /= 9.0;

    // Fade out towards the end of the last cascade.
    const float fade = clamp((splits.w - depth) / (splits.w * 0.1), 0.0, 1.0);
    return mix(1.0, lit, fade);
}
