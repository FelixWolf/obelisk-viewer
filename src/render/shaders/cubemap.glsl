// Cube map helpers for the probe filters.

const float CUBE_PI = 3.14159265359;

// The direction a cube face's texel (uv: 0..1, v down) stands for: the
// cube map sampling rules (major axis, then s and t across it) reversed.
// Faces: +X, -X, +Y, -Y, +Z, -Z.
vec3 cubeFaceDirection(int face, vec2 uv)
{
    const float s = uv.x * 2.0 - 1.0;
    const float t = uv.y * 2.0 - 1.0;
    vec3 d;
    switch(face)
    {
    case 0:
        d = vec3(1.0, -t, -s);
        break;
    case 1:
        d = vec3(-1.0, -t, s);
        break;
    case 2:
        d = vec3(s, 1.0, t);
        break;
    case 3:
        d = vec3(s, -1.0, -t);
        break;
    case 4:
        d = vec3(s, -t, 1.0);
        break;
    default:
        d = vec3(-s, -t, -1.0);
        break;
    }
    return normalize(d);
}

// Low-discrepancy points in the unit square (Hammersley).
vec2 hammersley(uint i, uint count)
{
    uint bits = (i << 16u) | (i >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return vec2(float(i) / float(count), float(bits) * 2.3283064365386963e-10);
}

// `local` (z along n) turned to world space around n.
vec3 aroundNormal(vec3 local, vec3 n)
{
    const vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    const vec3 tx = normalize(cross(up, n));
    const vec3 ty = cross(n, tx);
    return normalize(tx * local.x + ty * local.y + n * local.z);
}

// A GGX half vector for roughness r (alpha r squared).
vec3 sampleGgx(vec2 xi, float roughness, vec3 n)
{
    const float a = roughness * roughness;
    const float phi = 2.0 * CUBE_PI * xi.x;
    const float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    const float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    return aroundNormal(vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta), n);
}

float ggxDistribution(float nDotH, float roughness)
{
    const float a2 = roughness * roughness * roughness * roughness;
    const float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / (CUBE_PI * d * d);
}

// The mip level of a `size`-texel cube whose texels cover about the solid
// angle a sample of probability `pdf` (of `count`) stands for: fewer
// samples read blurrier levels, without fireflies.
float sampleLevel(float pdf, uint count, float size, float maxLevel)
{
    const float sampleAngle = 1.0 / (float(count) * pdf + 1e-6);
    const float texelAngle = 4.0 * CUBE_PI / (6.0 * size * size);
    return clamp(0.5 * log2(sampleAngle / texelAngle) + 1.0, 0.0, maxLevel);
}
