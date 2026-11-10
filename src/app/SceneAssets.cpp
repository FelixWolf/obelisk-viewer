#include "SceneAssets.hpp"

#include "Procedural.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace viewer
{

namespace
{

// A small heightfield for the test scene, `size` meters square starting at
// the origin, composition rising with height.
render::Mesh testTerrain(render::Renderer& renderer, float offsetX)
{
    constexpr uint32_t kCells = 64;
    constexpr float kSize = 32.0f;
    auto height = [offsetX](float x, float y)
    { return procedural::fbm((x + offsetX) * 0.08f, y * 0.08f, 4, 256) * 6.0f - 1.5f; };

    std::vector<render::TerrainVertex> vertices;
    for(uint32_t j = 0; j <= kCells; j++)
    {
        for(uint32_t i = 0; i <= kCells; i++)
        {
            const float x = static_cast<float>(i) * kSize / kCells;
            const float y = static_cast<float>(j) * kSize / kCells;
            const float h = height(x, y);
            const float d = 0.25f;
            const glm::vec3 normal =
                glm::normalize(glm::vec3(height(x - d, y) - height(x + d, y),
                                         height(x, y - d) - height(x, y + d), 2.0f * d));
            // Left to right as well as low to high, so all four layers show.
            const float composition = glm::clamp(x / kSize * 3.0f + (h - 1.5f) * 0.3f, 0.0f, 3.0f);
            vertices.push_back({{x, y, h}, normal, composition});
        }
    }
    std::vector<uint32_t> indices;
    for(uint32_t j = 0; j < kCells; j++)
    {
        for(uint32_t i = 0; i < kCells; i++)
        {
            const uint32_t a = j * (kCells + 1) + i;
            const uint32_t c = a + kCells + 1;
            indices.insert(indices.end(), {a, a + 1, c + 1, a, c + 1, c});
        }
    }
    return renderer.createMesh(std::span<const render::TerrainVertex>(vertices), indices);
}

template <size_t N>
render::GpuTexture uploadLayers(render::Renderer& renderer,
                                const std::array<image::TextureData, N>& layers, bool srgb)
{
    std::array<const image::TextureData*, N> pointers{};
    for(size_t i = 0; i < N; i++)
    {
        pointers[i] = &layers[i];
    }
    return renderer.uploadTextureArray(pointers, srgb);
}

render::GpuTexture uploadOne(render::Renderer& renderer, const image::TextureData& texture,
                             bool srgb)
{
    const image::TextureData* pointer = &texture;
    return std::move(renderer.uploadTextures({&pointer, 1}, srgb)[0]);
}

} // namespace

SceneAssets::SceneAssets(render::Renderer& renderer)
{
    const auto sphere = procedural::sphere();
    sphere_ =
        renderer.createMesh(std::span<const render::ObjectVertex>(sphere.vertices), sphere.indices);
    const auto cube = procedural::cube();
    cube_ = renderer.createMesh(std::span<const render::ObjectVertex>(cube.vertices), cube.indices);
    testTerrainLeft_ = testTerrain(renderer, 0.0f);
    testTerrainRight_ = testTerrain(renderer, 40.0f);

    checkerDiffuse_ = uploadOne(renderer, procedural::checkerDiffuse(), true);
    bumpNormal_ = uploadOne(renderer, procedural::bumpNormal(), false);
    stripeSpecular_ = uploadOne(renderer, procedural::stripeSpecular(), true);
    tileAlbedo_ = uploadOne(renderer, procedural::tileAlbedo(), true);
    tileOrm_ = uploadOne(renderer, procedural::tileOrm(), false);
    tileEmissive_ = uploadOne(renderer, procedural::tileEmissive(), true);
    tileNormal_ = uploadOne(renderer, procedural::tileNormal(), false);

    terrainDiffuse_ = uploadLayers(renderer, procedural::terrainDiffuse(), true);
    terrainOrm_ = uploadLayers(renderer, procedural::terrainOrm(), false);
    terrainNormal_ = uploadLayers(renderer, procedural::terrainNormal(), false);
}

render::TerrainBlinnPhongMaterial SceneAssets::terrainBlinnPhong() const
{
    return render::TerrainBlinnPhongMaterial{&terrainDiffuse_, 1.0f / 8.0f};
}

render::TerrainPbrMaterial SceneAssets::terrainPbr() const
{
    render::TerrainPbrMaterial m;
    m.albedoLayers = &terrainDiffuse_;
    m.ormLayers = &terrainOrm_;
    m.normalLayers = &terrainNormal_;
    m.textureRepeatsPerMeter = 1.0f / 8.0f;
    return m;
}

void SceneAssets::addTestScene(render::Scene& scene, glm::vec3 origin) const
{
    auto at = [&](float x, float y, float z, float scale = 1.0f)
    {
        return glm::scale(glm::translate(glm::mat4(1.0f), origin + glm::vec3(x, y, z)),
                          glm::vec3(scale));
    };

    // Back row: Blinn-Phong.
    render::BlinnPhongMaterial textured;
    textured.diffuse = &checkerDiffuse_;
    textured.normal = &bumpNormal_;
    textured.specular = &stripeSpecular_;
    scene.objects.push_back({&sphere_, at(-6.0f, 6.0f, 2.0f, 1.5f), textured});
    scene.objects.push_back({&cube_, at(-1.5f, 6.0f, 2.0f), textured});
    render::BlinnPhongMaterial plain;
    plain.diffuseTint = glm::vec4(0.8f, 0.1f, 0.1f, 1.0f);
    plain.maxExponent = 256.0f;
    scene.objects.push_back({&sphere_, at(3.0f, 6.0f, 2.0f, 1.5f), plain});
    render::BlinnPhongMaterial matte = plain;
    matte.diffuseTint = glm::vec4(0.2f, 0.6f, 0.2f, 1.0f);
    matte.specularTint = glm::vec3(0.0f);
    scene.objects.push_back({&sphere_, at(7.5f, 6.0f, 2.0f, 1.5f), matte});

    // Front row: PBR, a roughness sweep (dielectric, then metal) and tiles.
    for(int i = 0; i < 4; i++)
    {
        render::PbrMaterial m;
        m.baseColorFactor = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f);
        m.roughness = 0.1f + 0.3f * static_cast<float>(i);
        m.metallic = 0.0f;
        scene.objects.push_back(
            {&sphere_, at(-9.0f + 3.0f * static_cast<float>(i), 1.0f, 2.0f), m});
        m.baseColorFactor = glm::vec4(1.0f, 0.78f, 0.34f, 1.0f); // gold
        m.metallic = 1.0f;
        scene.objects.push_back(
            {&sphere_, at(-9.0f + 3.0f * static_cast<float>(i), -3.0f, 2.0f), m});
    }
    render::PbrMaterial tiles;
    tiles.albedo = &tileAlbedo_;
    tiles.orm = &tileOrm_;
    tiles.emissive = &tileEmissive_;
    tiles.normal = &tileNormal_;
    tiles.emissiveTint = glm::vec3(1.0f, 0.9f, 0.8f);
    tiles.emissiveStrength = 4.0f;
    scene.objects.push_back({&sphere_, at(5.0f, 1.0f, 2.0f, 1.5f), tiles});
    scene.objects.push_back({&cube_, at(9.0f, 1.0f, 2.0f), tiles});

    // Terrain below: classic on the left, PBR on the right.
    scene.terrain.push_back(
        {&testTerrainLeft_,
         glm::translate(glm::mat4(1.0f), origin + glm::vec3(-33.0f, -40.0f, 0.0f)),
         terrainBlinnPhong()});
    scene.terrain.push_back(
        {&testTerrainRight_,
         glm::translate(glm::mat4(1.0f), origin + glm::vec3(1.0f, -40.0f, 0.0f)), terrainPbr()});
}

} // namespace viewer
