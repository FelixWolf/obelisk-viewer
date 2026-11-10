#include "Procedural.hpp"

#include "viewer/image/TextureProcessing.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <functional>

namespace viewer::procedural
{

namespace
{

constexpr uint32_t kSize = 256;

uint8_t toByte(float v)
{
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Builds a texture from a function of (u, v) in [0, 1) returning RGBA 0..1.
image::TextureData makeTexture(const std::function<glm::vec4(float, float)>& f)
{
    image::MipLevel base;
    base.width = kSize;
    base.height = kSize;
    base.pixels.resize(kSize * kSize * 4);
    for(uint32_t y = 0; y < kSize; y++)
    {
        for(uint32_t x = 0; x < kSize; x++)
        {
            const glm::vec4 c =
                f((static_cast<float>(x) + 0.5f) / kSize, (static_cast<float>(y) + 0.5f) / kSize);
            uint8_t* p = &base.pixels[(y * kSize + x) * 4];
            p[0] = toByte(c.r);
            p[1] = toByte(c.g);
            p[2] = toByte(c.b);
            p[3] = toByte(c.a);
        }
    }
    image::TextureData texture;
    texture.levels = image::buildMipChain(std::move(base));
    return texture;
}

// Tangent-space normal map from a height function h(u, v) (in texels of
// height per texel of distance, scaled by `strength`). +Y follows +v.
image::TextureData normalFromHeight(const std::function<float(float, float)>& h, float strength,
                                    const std::function<float(float, float)>& alpha)
{
    const float d = 1.0f / kSize;
    return makeTexture(
        [&](float u, float v)
        {
            const float dx = (h(u + d, v) - h(u - d, v)) * strength;
            const float dy = (h(u, v + d) - h(u, v - d)) * strength;
            const glm::vec3 n = glm::normalize(glm::vec3(-dx, -dy, 1.0f));
            return glm::vec4(n * 0.5f + 0.5f, alpha(u, v));
        });
}

float hash(int x, int y)
{
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>(h ^ (h >> 16)) / 4294967295.0f;
}

// Tile pattern: 4x4 tiles with grout. Returns distance to the nearest grout
// line in tile units (0 at the grout, 0.5 in the middle).
float tileEdge(float u, float v)
{
    const float x = std::fmod(u * 4.0f, 1.0f);
    const float y = std::fmod(v * 4.0f, 1.0f);
    return std::min(std::min(x, 1.0f - x), std::min(y, 1.0f - y));
}

} // namespace

float noise(float x, float y, int period)
{
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int x0 = static_cast<int>(fx);
    const int y0 = static_cast<int>(fy);
    auto wrap = [period](int i) { return ((i % period) + period) % period; };
    const float tx = x - fx;
    const float ty = y - fy;
    const float sx = tx * tx * (3.0f - 2.0f * tx);
    const float sy = ty * ty * (3.0f - 2.0f * ty);
    const float a = hash(wrap(x0), wrap(y0));
    const float b = hash(wrap(x0 + 1), wrap(y0));
    const float c = hash(wrap(x0), wrap(y0 + 1));
    const float e = hash(wrap(x0 + 1), wrap(y0 + 1));
    return glm::mix(glm::mix(a, b, sx), glm::mix(c, e, sx), sy);
}

float fbm(float x, float y, int octaves, int period)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    float total = 0.0f;
    for(int i = 0; i < octaves; i++)
    {
        sum += noise(x, y, period) * amplitude;
        total += amplitude;
        x *= 2.0f;
        y *= 2.0f;
        period *= 2;
        amplitude *= 0.5f;
    }
    return sum / total;
}

MeshData sphere(uint32_t segments, uint32_t rings)
{
    MeshData mesh;
    const float pi = glm::pi<float>();
    for(uint32_t r = 0; r <= rings; r++)
    {
        const float v = static_cast<float>(r) / static_cast<float>(rings);
        const float theta = v * pi;
        for(uint32_t s = 0; s <= segments; s++)
        {
            const float u = static_cast<float>(s) / static_cast<float>(segments);
            const float phi = u * 2.0f * pi;
            render::ObjectVertex vertex;
            vertex.normal = glm::vec3(std::cos(phi) * std::sin(theta),
                                      std::sin(phi) * std::sin(theta), std::cos(theta));
            vertex.position = vertex.normal;
            // Tangent along +u; cross(N, T) points along -v, so w = -1 makes
            // the bitangent follow +v.
            vertex.tangent = glm::vec4(-std::sin(phi), std::cos(phi), 0.0f, -1.0f);
            vertex.uv = glm::vec2(u * 2.0f, v); // repeat twice around
            mesh.vertices.push_back(vertex);
        }
    }
    for(uint32_t r = 0; r < rings; r++)
    {
        for(uint32_t s = 0; s < segments; s++)
        {
            const uint32_t a = r * (segments + 1) + s;
            const uint32_t b = a + segments + 1;
            // Counter-clockwise seen from outside.
            mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
    return mesh;
}

MeshData cube()
{
    MeshData mesh;
    struct Face
    {
        glm::vec3 normal;
        glm::vec3 tangent;
    };
    const Face faces[] = {
        {{1, 0, 0}, {0, 1, 0}},  {{-1, 0, 0}, {0, -1, 0}}, {{0, 1, 0}, {-1, 0, 0}},
        {{0, -1, 0}, {1, 0, 0}}, {{0, 0, 1}, {1, 0, 0}},   {{0, 0, -1}, {-1, 0, 0}},
    };
    for(const Face& f : faces)
    {
        const glm::vec3 bitangent = glm::cross(f.normal, f.tangent);
        const auto base = static_cast<uint32_t>(mesh.vertices.size());
        const glm::vec2 corners[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for(const glm::vec2& c : corners)
        {
            render::ObjectVertex vertex;
            vertex.position =
                f.normal + f.tangent * (c.x * 2.0f - 1.0f) + bitangent * (c.y * 2.0f - 1.0f);
            vertex.normal = f.normal;
            vertex.tangent = glm::vec4(f.tangent, 1.0f);
            vertex.uv = c;
            mesh.vertices.push_back(vertex);
        }
        mesh.indices.insert(mesh.indices.end(),
                            {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return mesh;
}

MeshData quad()
{
    MeshData mesh;
    const glm::vec2 corners[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for(const glm::vec2& c : corners)
    {
        render::ObjectVertex v;
        v.position = glm::vec3(c, 0.0f);
        v.normal = glm::vec3(0, 0, 1);
        v.tangent = glm::vec4(1, 0, 0, 1);
        v.uv = c;
        mesh.vertices.push_back(v);
    }
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

image::TextureData waterNormal()
{
    return normalFromHeight([](float u, float v) { return fbm(u * 8.0f, v * 8.0f, 4, 8); }, 1.5f,
                            [](float, float) { return 1.0f; });
}

image::TextureData checkerDiffuse()
{
    return makeTexture(
        [](float u, float v)
        {
            const bool odd = (static_cast<int>(u * 8.0f) + static_cast<int>(v * 8.0f)) % 2 != 0;
            return odd ? glm::vec4(0.75f, 0.32f, 0.2f, 1.0f) : glm::vec4(0.9f, 0.85f, 0.75f, 1.0f);
        });
}

image::TextureData bumpNormal()
{
    const float twoPi = glm::two_pi<float>();
    return normalFromHeight([twoPi](float u, float v)
                            { return std::sin(u * twoPi * 8.0f) * std::sin(v * twoPi * 8.0f); },
                            6.0f, [](float, float) { return 1.0f; });
}

image::TextureData stripeSpecular()
{
    return makeTexture(
        [](float u, float)
        {
            const float s = static_cast<int>(u * 8.0f) % 2 == 0 ? 1.0f : 0.15f;
            return glm::vec4(s, s, s, 1.0f);
        });
}

image::TextureData tileAlbedo()
{
    return makeTexture(
        [](float u, float v)
        {
            if(tileEdge(u, v) < 0.04f)
            {
                return glm::vec4(0.35f, 0.33f, 0.3f, 1.0f);
            }
            const float shade = 0.85f + 0.15f * fbm(u * 16.0f, v * 16.0f, 3, 16);
            return glm::vec4(0.2f * shade, 0.45f * shade, 0.75f * shade, 1.0f);
        });
}

image::TextureData tileOrm()
{
    return makeTexture(
        [](float u, float v)
        {
            const float edge = tileEdge(u, v);
            const bool grout = edge < 0.04f;
            const float occlusion = grout ? 0.5f : std::min(1.0f, 0.75f + edge);
            return glm::vec4(occlusion, grout ? 0.9f : 0.25f, 0.0f, 1.0f);
        });
}

image::TextureData tileEmissive()
{
    return makeTexture(
        [](float u, float v)
        {
            const float x = std::fmod(u * 4.0f, 1.0f) - 0.5f;
            const float y = std::fmod(v * 4.0f, 1.0f) - 0.5f;
            const float glow = std::sqrt(x * x + y * y) < 0.12f ? 1.0f : 0.0f;
            return glm::vec4(glow, glow * 0.6f, glow * 0.2f, 1.0f);
        });
}

image::TextureData tileNormal()
{
    return normalFromHeight([](float u, float v) { return std::min(tileEdge(u, v), 0.08f) * 4.0f; },
                            8.0f, [](float, float) { return 1.0f; });
}

std::array<image::TextureData, 4> terrainDiffuse()
{
    const glm::vec3 colors[4] = {
        {0.38f, 0.27f, 0.17f}, // dirt
        {0.22f, 0.38f, 0.12f}, // grass
        {0.42f, 0.4f, 0.37f},  // rock
        {0.9f, 0.92f, 0.95f},  // snow
    };
    std::array<image::TextureData, 4> layers;
    for(int i = 0; i < 4; i++)
    {
        layers[static_cast<size_t>(i)] = makeTexture(
            [&, i](float u, float v)
            {
                const float n = fbm(u * 8.0f + static_cast<float>(i) * 17.0f, v * 8.0f, 5, 8);
                return glm::vec4(colors[i] * (0.6f + 0.8f * n), 1.0f);
            });
    }
    return layers;
}

std::array<image::TextureData, 4> terrainOrm()
{
    const float roughness[4] = {0.95f, 0.85f, 0.6f, 0.35f};
    std::array<image::TextureData, 4> layers;
    for(int i = 0; i < 4; i++)
    {
        layers[static_cast<size_t>(i)] = makeTexture(
            [&, i](float u, float v)
            {
                const float n = fbm(u * 8.0f + static_cast<float>(i) * 17.0f, v * 8.0f, 5, 8);
                return glm::vec4(0.7f + 0.3f * n, roughness[i], 0.0f, 1.0f);
            });
    }
    return layers;
}

std::array<image::TextureData, 4> terrainNormal()
{
    const float strength[4] = {3.0f, 2.0f, 6.0f, 1.0f};
    std::array<image::TextureData, 4> layers;
    for(int i = 0; i < 4; i++)
    {
        layers[static_cast<size_t>(i)] = normalFromHeight(
            [i](float u, float v)
            { return fbm(u * 8.0f + static_cast<float>(i) * 17.0f, v * 8.0f, 5, 8); }, strength[i],
            [](float, float) { return 1.0f; });
    }
    return layers;
}

} // namespace viewer::procedural
