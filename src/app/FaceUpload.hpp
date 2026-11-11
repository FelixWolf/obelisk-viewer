#pragma once

// Uploads volume faces (generated prims, decoded meshes) as GPU meshes, with
// tangents computed for normal maps. Faces without geometry come back empty,
// so face i stays texture entry i. Rigged faces (with joint weights) become
// skinned meshes.

#include "viewer/render/Renderer.hpp"
#include "viewer/volume/Volume.hpp"

#include <vector>

namespace viewer
{

std::vector<render::Mesh> uploadFaces(render::Renderer& renderer,
                                      const std::vector<volume::VolumeFace>& faces);

} // namespace viewer
