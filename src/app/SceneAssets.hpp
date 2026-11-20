#pragma once

// GPU resources built from procedural content: the material test scene and
// placeholder terrain textures (until terrain textures come from the grid).

#include "viewer/render/Renderer.hpp"
#include "viewer/render/Scene.hpp"

namespace viewer
{

class SceneAssets
{
public:
    explicit SceneAssets(render::Renderer& renderer);

    // Placeholder terrain materials (dirt, grass, rock, snow).
    render::TerrainBlinnPhongMaterial terrainBlinnPhong() const;
    render::TerrainPbrMaterial terrainPbr() const;

    // Adds the material test scene: Blinn-Phong and PBR spheres and cubes,
    // and a patch of terrain drawn with each terrain shader. Centered on
    // `origin`.
    void addTestScene(render::Scene& scene, glm::vec3 origin) const;
    // The login lobby's floor until it has a scene of its own: a plain
    // slab, its top at z = 0, `size` meters square around `center`.
    void addFloor(render::Scene& scene, glm::vec2 center, float size) const;

private:
    render::Mesh sphere_;
    render::Mesh cube_;
    render::Mesh testTerrainLeft_;
    render::Mesh testTerrainRight_;

    render::GpuTexture checkerDiffuse_;
    render::GpuTexture bumpNormal_;
    render::GpuTexture stripeSpecular_;
    render::GpuTexture tileAlbedo_;
    render::GpuTexture tileOrm_;
    render::GpuTexture tileEmissive_;
    render::GpuTexture tileNormal_;

    render::GpuTexture terrainDiffuse_;
    render::GpuTexture terrainOrm_;
    render::GpuTexture terrainNormal_;
};

} // namespace viewer
