#version 450

// FXAA (after Timothy Lottes' FXAA 3.11, the quality version, its preset
// from the strength: 12, 25 or 39, Renderer::setFxaa):
// the tonemapped view (sRGB-encoded, so luma is perceptual) smoothed along
// the edges it finds, in place of multisampling. Finds each pixel's edge by
// its neighbours' luma, walks along it both ways to where it ends, and
// samples across it by how far the pixel is from the nearer end; small
// features also get a blur by how much they stand out (subpixel aliasing).

layout(location = 0) in vec2 inUv;
layout(set = 2, binding = 0) uniform sampler2D view; // linear, clamped
layout(set = 3, binding = 0) uniform FxaaFS
{
    // x contrast taken as an edge, of the brightest; y and at least this
    // (dark areas aren't touched); z how much subpixel aliasing is removed;
    // w how many steps along an edge (of the 12)
    vec4 settings;
    vec4 steps[3]; // each step's length, in pixels
};
layout(location = 0) out vec4 outColor;

float stepLength(int i)
{
    return steps[i / 4][i % 4];
}

float luma(vec3 c)
{
    return dot(c, vec3(0.299, 0.587, 0.114));
}
float lumaAt(vec2 uv)
{
    return luma(textureLod(view, uv, 0.0).rgb);
}
vec2 rcp;
float lumaOff(vec2 uv, ivec2 offset)
{
    return lumaAt(uv + vec2(offset) * rcp);
}

void main()
{
    rcp = 1.0 / vec2(textureSize(view, 0));
    vec2 posM = inUv;
    const vec3 rgbM = textureLod(view, posM, 0.0).rgb;
    const float lumaM = luma(rgbM);
    float lumaS = lumaOff(posM, ivec2(0, 1));
    const float lumaE = lumaOff(posM, ivec2(1, 0));
    float lumaN = lumaOff(posM, ivec2(0, -1));
    const float lumaW = lumaOff(posM, ivec2(-1, 0));

    const float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    const float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    const float range = rangeMax - rangeMin;
    if(range < max(settings.y, rangeMax * settings.x))
    {
        outColor = vec4(rgbM, 1.0);
        return;
    }

    const float lumaNW = lumaOff(posM, ivec2(-1, -1));
    const float lumaSE = lumaOff(posM, ivec2(1, 1));
    const float lumaNE = lumaOff(posM, ivec2(1, -1));
    const float lumaSW = lumaOff(posM, ivec2(-1, 1));

    // Which way the edge runs: horizontal or vertical.
    const float lumaNS = lumaN + lumaS;
    const float lumaWE = lumaW + lumaE;
    const float lumaNESE = lumaNE + lumaSE;
    const float lumaNWNE = lumaNW + lumaNE;
    const float lumaNWSW = lumaNW + lumaSW;
    const float lumaSWSE = lumaSW + lumaSE;
    const float edgeHorz = abs(-2.0 * lumaW + lumaNWSW) + abs(-2.0 * lumaM + lumaNS) * 2.0 +
                           abs(-2.0 * lumaE + lumaNESE);
    const float edgeVert = abs(-2.0 * lumaS + lumaSWSE) + abs(-2.0 * lumaM + lumaWE) * 2.0 +
                           abs(-2.0 * lumaN + lumaNWNE);
    const bool horzSpan = edgeHorz >= edgeVert;

    // Subpixel blend: how much the pixel differs from its neighbourhood.
    const float subpixA = (lumaNS + lumaWE) * 2.0 + lumaNWSW + lumaNESE;
    const float subpixC = clamp(abs(subpixA / 12.0 - lumaM) / range, 0.0, 1.0);
    const float subpixF = (-2.0 * subpixC + 3.0) * subpixC * subpixC;

    // Which side of the pixel the edge is on.
    if(!horzSpan)
    {
        lumaN = lumaW;
        lumaS = lumaE;
    }
    float lengthSign = horzSpan ? rcp.y : rcp.x;
    const float gradientN = lumaN - lumaM;
    const float gradientS = lumaS - lumaM;
    const bool pairN = abs(gradientN) >= abs(gradientS);
    const float gradient = max(abs(gradientN), abs(gradientS));
    if(pairN)
    {
        lengthSign = -lengthSign;
    }
    const float lumaEdge = ((pairN ? lumaN : lumaS) + lumaM) * 0.5;

    // Walk along the edge, between this pixel and the one across it, both
    // ways until the luma there leaves the edge's.
    vec2 posB = posM;
    if(horzSpan)
    {
        posB.y += lengthSign * 0.5;
    }
    else
    {
        posB.x += lengthSign * 0.5;
    }
    const vec2 offNP = horzSpan ? vec2(rcp.x, 0.0) : vec2(0.0, rcp.y);
    vec2 posN = posB - offNP * stepLength(0);
    vec2 posP = posB + offNP * stepLength(0);
    const float gradientScaled = gradient * 0.25;
    float lumaEndN = lumaAt(posN) - lumaEdge;
    float lumaEndP = lumaAt(posP) - lumaEdge;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    const int count = int(settings.w);
    for(int i = 1; i < count && !(doneN && doneP); i++)
    {
        if(!doneN)
        {
            posN -= offNP * stepLength(i);
            lumaEndN = lumaAt(posN) - lumaEdge;
            doneN = abs(lumaEndN) >= gradientScaled;
        }
        if(!doneP)
        {
            posP += offNP * stepLength(i);
            lumaEndP = lumaAt(posP) - lumaEdge;
            doneP = abs(lumaEndP) >= gradientScaled;
        }
    }

    // Sample across the edge by how far the nearer end is: half a pixel at
    // an end, nothing in the middle of a long edge.
    const float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y;
    const float dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;
    const bool lumaMLTZero = lumaM - lumaEdge < 0.0;
    const bool directionN = dstN < dstP;
    const bool goodSpan = ((directionN ? lumaEndN : lumaEndP) < 0.0) != lumaMLTZero;
    const float pixelOffset = goodSpan ? 0.5 - min(dstN, dstP) / (dstN + dstP) : 0.0;
    const float offset = max(pixelOffset, subpixF * subpixF * settings.z);
    if(horzSpan)
    {
        posM.y += offset * lengthSign;
    }
    else
    {
        posM.x += offset * lengthSign;
    }
    outColor = vec4(textureLod(view, posM, 0.0).rgb, 1.0);
}
