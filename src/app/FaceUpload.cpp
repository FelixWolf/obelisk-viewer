#include "FaceUpload.hpp"

#include <cmath>

namespace viewer
{

namespace
{

// Tangents along +u (Lengyel's method), for normal maps.
std::vector<glm::vec4> tangents(const volume::VolumeFace& face)
{
    std::vector<glm::vec3> tan(face.positions.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> bitan(face.positions.size(), glm::vec3(0.0f));
    for(size_t i = 0; i + 2 < face.indices.size(); i += 3)
    {
        const uint32_t i0 = face.indices[i];
        const uint32_t i1 = face.indices[i + 1];
        const uint32_t i2 = face.indices[i + 2];
        const glm::vec3 e1 = face.positions[i1] - face.positions[i0];
        const glm::vec3 e2 = face.positions[i2] - face.positions[i0];
        const glm::vec2 d1 = face.texCoords[i1] - face.texCoords[i0];
        const glm::vec2 d2 = face.texCoords[i2] - face.texCoords[i0];
        const float det = d1.x * d2.y - d2.x * d1.y;
        if(std::abs(det) < 1e-12f)
        {
            continue;
        }
        const float r = 1.0f / det;
        const glm::vec3 sdir = (e1 * d2.y - e2 * d1.y) * r;
        const glm::vec3 tdir = (e2 * d1.x - e1 * d2.x) * r;
        for(const uint32_t v : {i0, i1, i2})
        {
            tan[v] += sdir;
            bitan[v] += tdir;
        }
    }
    std::vector<glm::vec4> out(face.positions.size());
    for(size_t v = 0; v < out.size(); v++)
    {
        const glm::vec3& n = face.normals[v];
        glm::vec3 t = tan[v] - n * glm::dot(n, tan[v]);
        if(glm::dot(t, t) < 1e-12f)
        {
            out[v] = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
            continue;
        }
        t = glm::normalize(t);
        const float handedness = glm::dot(glm::cross(n, t), bitan[v]) < 0.0f ? -1.0f : 1.0f;
        out[v] = glm::vec4(t, handedness);
    }
    return out;
}

} // namespace

std::vector<render::Mesh> uploadFaces(render::Renderer& renderer,
                                      const std::vector<volume::VolumeFace>& faces)
{
    std::vector<render::Mesh> meshes;
    meshes.reserve(faces.size());
    for(const volume::VolumeFace& face : faces)
    {
        if(face.indices.empty())
        {
            meshes.emplace_back();
            continue;
        }
        const auto tan = tangents(face);
        if(!face.weights.empty())
        {
            std::vector<render::SkinnedVertex> vertices(face.positions.size());
            for(size_t v = 0; v < vertices.size(); v++)
            {
                vertices[v] = {face.positions[v], face.normals[v], tan[v],
                               face.texCoords[v], face.joints[v],  face.weights[v]};
            }
            meshes.push_back(renderer.createMesh(std::span<const render::SkinnedVertex>(vertices),
                                                 face.indices));
            continue;
        }
        std::vector<render::ObjectVertex> vertices(face.positions.size());
        for(size_t v = 0; v < vertices.size(); v++)
        {
            vertices[v] = {face.positions[v], face.normals[v], tan[v], face.texCoords[v]};
        }
        meshes.push_back(
            renderer.createMesh(std::span<const render::ObjectVertex>(vertices), face.indices));
    }
    return meshes;
}

} // namespace viewer
