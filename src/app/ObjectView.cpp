#include "ObjectView.hpp"

#include "FaceUpload.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <bit>
#include <cmath>
#include <thread>

namespace viewer
{

namespace
{

// Faces whose texture is the official viewer's fully transparent one aren't
// drawn (asset id from indra_constants.cpp).
const Uuid kTransparentTexture = *Uuid::parse("8dcd4a48-2d37-4909-9f78-f7a9eb4ef903");

// Level of detail from how large the prim appears, as the official viewer's
// LLVolumeLODGroup::getDetailFromTan with its default thresholds and a LOD
// factor of 1.25.
int detailFor(float radius, float distance)
{
    constexpr float kThresholds[] = {0.03f, 0.06f, 0.24f, 3.0f};
    constexpr float kLodFactor = 1.25f;
    const float tanAngle = radius / std::max(distance, 0.01f) * kLodFactor;
    for(int i = 0; i < 4; i++)
    {
        if(tanAngle <= kThresholds[i])
        {
            return i;
        }
    }
    return 3;
}

// Texture entry transform as the official viewer's llface.cpp xform(): about
// the face's center, rotate, then scale, then offset.
// A glTF KHR_texture_transform for Second Life texture coordinates: into
// glTF's convention (v down), transformed, and back (as the official
// viewer's texture_transform in textureUtilV.glsl).
void gltfUvTransform(const assets::TextureTransform& t, glm::vec3& row0, glm::vec3& row1)
{
    const glm::mat3 flip(1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    const float c = std::cos(t.rotation);
    const float s = std::sin(t.rotation);
    const glm::mat3 scale(t.scale.x, 0.0f, 0.0f, 0.0f, t.scale.y, 0.0f, 0.0f, 0.0f, 1.0f);
    const glm::mat3 rotate(c, -s, 0.0f, s, c, 0.0f, 0.0f, 0.0f, 1.0f);
    const glm::mat3 offset(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, t.offset.x, t.offset.y, 1.0f);
    const glm::mat3 m = flip * offset * rotate * scale * flip;
    row0 = glm::vec3(m[0][0], m[1][0], m[2][0]);
    row1 = glm::vec3(m[0][1], m[1][1], m[2][1]);
}

glm::vec3 srgbToLinear(glm::vec3 c)
{
    auto channel = [](float v)
    { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return glm::vec3(channel(c.r), channel(c.g), channel(c.b));
}

void uvTransform(glm::vec2 repeats, glm::vec2 offset, float rotation, glm::vec3& row0,
                 glm::vec3& row1)
{
    const float c = std::cos(rotation);
    const float s = std::sin(rotation);
    const float a = c * repeats.x;
    const float b = s * repeats.x;
    const float d = -s * repeats.y;
    const float e = c * repeats.y;
    row0 = glm::vec3(a, b, -0.5f * a - 0.5f * b + offset.x + 0.5f);
    row1 = glm::vec3(d, e, -0.5f * d - 0.5f * e + offset.y + 0.5f);
}

void uvTransform(const world::TextureFace& face, glm::vec3& row0, glm::vec3& row1)
{
    uvTransform(face.repeats, face.offset, face.rotation, row0, row1);
}

} // namespace

size_t ObjectView::KeyHash::operator()(const Key& key) const
{
    uint64_t h = 1469598103934665603ull; // FNV-1a over the fields' bits
    const auto mix = [&](uint32_t bits) { h = (h ^ bits) * 1099511628211ull; };
    const auto mixFloat = [&](float f) { mix(std::bit_cast<uint32_t>(f)); };
    const volume::ProfileParams& profile = key.params.profile;
    const volume::PathParams& path = key.params.path;
    mix(profile.curve);
    mixFloat(profile.begin);
    mixFloat(profile.end);
    mixFloat(profile.hollow);
    mix(path.curve);
    for(const float f : {path.begin, path.end, path.scale.x, path.scale.y, path.shear.x,
                         path.shear.y, path.twistBegin, path.twist, path.radiusOffset, path.taper.x,
                         path.taper.y, path.revolutions, path.skew})
    {
        mixFloat(f);
    }
    mix(static_cast<uint32_t>(key.detail));
    return static_cast<size_t>(h);
}

ObjectView::ObjectView(JobSystem& jobs, render::Renderer& renderer, const ObjectMotion& motion)
    : jobs_(jobs), renderer_(renderer), motion_(motion)
{
}

ObjectView::~ObjectView()
{
    // Jobs push into generated_; wait until none is running.
    while(running_.load() > 0)
    {
        std::this_thread::yield();
    }
}

void ObjectView::clear()
{
    cache_.clear();
}

std::optional<glm::mat4> ObjectView::frameOf(const world::Object& object, const world::World& world,
                                             uint64_t originHandle, const AvatarView* avatars,
                                             int depth) const
{
    const glm::mat4 local = motion_.local(object);
    if(object.parentId == 0)
    {
        const glm::vec3 offset(
            static_cast<float>(static_cast<int64_t>(object.region >> 32) -
                               static_cast<int64_t>(originHandle >> 32)),
            static_cast<float>(static_cast<int64_t>(object.region & 0xFFFFFFFFu) -
                               static_cast<int64_t>(originHandle & 0xFFFFFFFFu)),
            0.0f);
        return glm::translate(glm::mat4(1.0f), offset) * local;
    }
    const world::Object* parent = world.object(object.region, object.parentId);
    if(!parent || depth > 8)
    {
        return std::nullopt;
    }
    if(parent->type == world::ObjectType::Avatar)
    {
        // The attachment point is in the attached root's state byte.
        if(!avatars)
        {
            return std::nullopt;
        }
        const auto point = avatars->attachmentFrame(
            *parent, world, avatar::attachmentPointFromState(object.state), originHandle);
        return point ? std::optional(*point * local) : std::nullopt;
    }
    const auto parentFrame = frameOf(*parent, world, originHandle, avatars, depth + 1);
    return parentFrame ? std::optional(*parentFrame * local) : std::nullopt;
}

ObjectView::Generated ObjectView::withExtent(Generated g)
{
    g.extent = glm::vec3(0.0f);
    for(const volume::VolumeFace& face : g.faces)
    {
        for(const glm::vec3& p : face.positions)
        {
            g.extent = glm::max(g.extent, glm::abs(p));
        }
    }
    return g;
}

ObjectView::Geometry& ObjectView::geometry(const Key& key)
{
    auto [it, inserted] = cache_.try_emplace(key);
    if(inserted)
    {
        running_++;
        jobs_.submit(
            [this, key]
            {
                generated_.push(withExtent(Generated{
                    key, volume::generateVolume(
                             key.params, volume::kDetailScales[static_cast<size_t>(key.detail)])}));
                running_--;
            });
    }
    return it->second;
}

void ObjectView::update()
{
    generated_.drain(drained_);
    for(Generated& g : drained_)
    {
        Geometry& geo = cache_[g.key];
        geo.faces = uploadFaces(renderer_, g.faces);
        geo.extent = g.extent;
        geo.ready = true;
    }
    drained_.clear();
}

std::optional<render::BlinnPhongMaterial>
ObjectView::material(const world::TextureFace& face, TextureManager& textures,
                     LegacyMaterialManager& legacyMaterials, const world::World& world,
                     const world::Object* wearer, const AvatarView* avatars, float texels)
{
    render::BlinnPhongMaterial m;
    if(wearer && avatars)
    {
        // Worn on the body: shown about as big as the avatar's own bakes.
        if(const auto bake =
               avatars->bakeFor(world, *wearer, face.texture, textures, 2.0f * texels))
        {
            if(!*bake)
            {
                return std::nullopt;
            }
            // Bakes cut holes where alpha layers are, like the body.
            m.diffuse = *bake;
            m.diffuseTint = face.color;
            m.alphaCutoff = 0.5f;
            m.specularTint = glm::vec3(0.0f); // bodies have no highlight
            return m;
        }
    }
    auto texelsAt = [&](glm::vec2 repeats)
    { return texels * std::max(std::abs(repeats.x), std::abs(repeats.y)); };
    m.diffuse = textures.get(face.texture, texelsAt(face.repeats));
    m.diffuseTint = face.color;
    m.fullbright = face.fullbright();
    // Legacy faces have no highlight, as in the official viewer. "Shiny"
    // there reflects the environment (low, medium, high); without
    // environment reflections yet, a soft broad sheen stands in.
    m.specularTint = glm::vec3(0.04f * static_cast<float>(face.shiny()));
    m.maxExponent = 16.0f;
    auto alphaMode = assets::DiffuseAlphaMode::Default;
    float maskCutoff = 0.5f;
    // A legacy material: normal and specular maps with their own
    // transforms, specular color and glossiness, and what the diffuse
    // alpha means (as the official viewer's materialF.glsl).
    if(const auto legacy = legacyMaterials.get(face.material))
    {
        m.mapTransforms = true;
        uvTransform(legacy->normalTransform.scale, legacy->normalTransform.offset,
                    legacy->normalTransform.rotation, m.normalUv[0], m.normalUv[1]);
        uvTransform(legacy->specularTransform.scale, legacy->specularTransform.offset,
                    legacy->specularTransform.rotation, m.specularUv[0], m.specularUv[1]);
        if(!legacy->normalMap.isNull())
        {
            m.normal =
                textures.getLinear(legacy->normalMap, texelsAt(legacy->normalTransform.scale));
        }
        if(!legacy->specularMap.isNull())
        {
            m.specular =
                textures.get(legacy->specularMap, texelsAt(legacy->specularTransform.scale));
        }
        // Exponent: glossiness squared times RenderSpecularExponent (368),
        // as the official specular table; none without glossiness.
        m.specularTint = legacy->glossiness > 0.0f ? srgbToLinear(glm::vec3(legacy->specularColor))
                                                   : glm::vec3(0.0f);
        m.maxExponent = 368.0f * legacy->glossiness * legacy->glossiness;
        alphaMode = legacy->alphaMode;
        maskCutoff = legacy->alphaCutoff;
    }
    switch(alphaMode)
    {
    case assets::DiffuseAlphaMode::None:
        m.opaque = true;
        break;
    case assets::DiffuseAlphaMode::Blend:
        m.blend = true;
        break;
    case assets::DiffuseAlphaMode::Mask:
        m.alphaCutoff = maskCutoff;
        break;
    case assets::DiffuseAlphaMode::Emissive:
        m.emissiveMask = true;
        m.opaque = true;
        break;
    case assets::DiffuseAlphaMode::Default:
    {
        const auto alpha = textures.alphaMode(face.texture);
        if(alpha == image::AlphaMode::Mask)
        {
            m.alphaCutoff = 0.5f;
        }
        m.blend = alpha == image::AlphaMode::Blend;
        break;
    }
    }
    m.blend = m.blend || face.color.a < 1.0f;
    if(m.blend)
    {
        m.opaque = false;
    }
    return m;
}

bool ObjectView::pbrMaterial(render::ObjectDraw& draw, const world::Object& object, size_t face,
                             const world::World& world, TextureManager& textures,
                             MaterialManager& materials, float texels)
{
    Uuid id;
    for(const auto& [f, material] : object.materials)
    {
        id = f == face ? material : id;
    }
    if(id.isNull())
    {
        return false;
    }
    const auto base = materials.get(id);
    if(!base)
    {
        return false;
    }
    assets::Material m = *base;
    if(const world::FaceOverrides* overrides =
           world.materialOverrides(world::ObjectKey{object.region, object.localId}))
    {
        for(const auto& [f, change] : *overrides)
        {
            if(f == face)
            {
                m = assets::applyOverride(std::move(m), change);
            }
        }
    }
    render::PbrMaterial p;
    // Each map's size from its own repeats.
    auto texelsOf = [&](size_t slot)
    {
        const glm::vec2 scale = m.transforms[slot].scale;
        return texels * std::max(std::abs(scale.x), std::abs(scale.y));
    };
    p.albedo =
        textures.get(m.textures[assets::kBaseColorTexture], texelsOf(assets::kBaseColorTexture));
    p.normal =
        textures.getLinear(m.textures[assets::kNormalTexture], texelsOf(assets::kNormalTexture));
    // glTF's metallic-roughness map: G roughness, B metallic (R unused).
    p.orm = textures.getLinear(m.textures[assets::kMetallicRoughnessTexture],
                               texelsOf(assets::kMetallicRoughnessTexture));
    p.emissive =
        textures.get(m.textures[assets::kEmissiveTexture], texelsOf(assets::kEmissiveTexture));
    p.baseColorFactor = m.baseColor;
    p.emissiveTint = m.emissive;
    p.emissiveStrength = 1.0f;
    p.roughness = m.roughness;
    p.metallic = m.metallic;
    p.occlusionStrength = 0.0f;
    p.alphaCutoff = m.alphaMode == assets::MaterialAlpha::Mask ? m.alphaCutoff : -1.0f;
    p.blend = m.alphaMode == assets::MaterialAlpha::Blend;
    p.doubleSided = m.doubleSided;
    draw.material = p;
    // PBR faces use the material's transforms, not the legacy repeats (the
    // base color's for every map, for now).
    gltfUvTransform(m.transforms[assets::kBaseColorTexture], draw.uvRow0, draw.uvRow1);
    return true;
}

void ObjectView::addLight(render::Scene& scene, const world::World& world,
                          const world::Object& object, uint64_t originHandle,
                          TextureManager& textures, const AvatarView* avatars,
                          float drawDistance) const
{
    const world::LightParams& light = *object.light;
    if(light.intensity <= 0.0f || light.radius <= 0.0f)
    {
        return;
    }
    const auto frame = frameOf(object, world, originHandle, avatars);
    if(!frame)
    {
        return;
    }
    // As the official viewer's deferred lights (pipeline.cpp): 1.5 times the
    // radius, half the falloff.
    render::LocalLight out;
    out.position = glm::vec3((*frame)[3]);
    out.range = light.radius * 1.5f;
    if(glm::length(out.position - scene.camera.position) - out.range > drawDistance)
    {
        return;
    }
    out.color = light.color * light.intensity;
    out.falloff = light.falloff * 0.5f;
    // A light with a texture is a projector (llprimitive.h isLightSpotlight),
    // shining along the prim's -Z.
    if(object.lightImage && !object.lightImage->texture.isNull())
    {
        const world::LightImage& image = *object.lightImage;
        out.projector =
            render::projectorFor(out.position, glm::quat_cast(glm::mat3(*frame)), object.scale,
                                 out.range, image.fov, image.focus, image.ambiance);
        out.projector->texture = textures.get(image.texture, 512.0f); // the projector array's size
    }
    scene.lights.push_back(out);
}

void ObjectView::addRigged(render::Scene& scene, const world::World& world,
                           const world::Object& object, uint64_t originHandle,
                           TextureManager& textures, MeshManager& meshes,
                           MaterialManager& materials, LegacyMaterialManager& legacyMaterials,
                           const AvatarView* avatars, float drawDistance)
{
    const SkeletonOwner owner = avatars ? skeletonOwner(world, object) : SkeletonOwner{};
    if(!owner.owner)
    {
        stats_.skipped++; // not worn, and not part of an animated object
        return;
    }
    glm::mat4 root(1.0f);
    float radius = 0.0f;
    if(owner.animated)
    {
        // An animated object's skeleton stands where its root prim is
        // (llcontrolavatar.cpp matchVolumeTransform): turned by the root's
        // bind shape rotation too unless worn.
        const auto frame = frameOf(*owner.root, world, originHandle, avatars);
        if(!frame)
        {
            stats_.skipped++;
            return;
        }
        root = *frame;
        if(owner.root->parentId == 0 &&
           (owner.root->sculptType & world::SculptType::Mask) == world::SculptType::Mesh)
        {
            if(const auto rootSkin = meshes.skin(owner.root->sculptTexture))
            {
                const glm::mat3 bind(rootSkin->bindShape);
                root *= glm::mat4(glm::mat3_cast(glm::quat_cast(glm::mat3(
                    glm::normalize(bind[0]), glm::normalize(bind[1]), glm::normalize(bind[2])))));
            }
        }
        // Its extent is used as a radius, unlike an avatar's (LLVOVolume::calcLOD).
        radius = 0.5f * avatars->extent(*owner.owner);
    }
    else
    {
        root = avatars->rootFrame(*owner.owner, world, originHandle);
        radius = avatars->extent(*owner.owner);
    }
    const float distance = glm::length(glm::vec3(root[3]) - scene.camera.position);
    if(distance > drawDistance)
    {
        return;
    }
    // Rigged mesh reaches past the skeleton (hair, tails, skirts): a margin.
    const render::Visibility seen = scene.visibility(glm::vec3(root[3]), radius + 1.0f);
    if(seen == render::Visibility::Hidden)
    {
        stats_.culled++;
        return;
    }
    // Textures as big as the skeleton shows (the least for shadows only).
    const float texels =
        seen == render::Visibility::ShadowOnly ? 0.0f : textures.texelsFor(2.0f * radius, distance);
    const auto skin = meshes.skin(object.sculptTexture);
    // Rigged mesh is skeleton sized, whatever the object's scale says: as the
    // official viewer (LLVOVolume::calcLOD), its level of detail comes from
    // the skeleton's extent.
    const auto* faces = meshes.get(object.sculptTexture, detailFor(radius, distance));
    if(!skin || !faces)
    {
        return;
    }

    // The matrices the vertices' joints refer to: from the mesh's bind pose
    // to the joint as posed now (joints the avatar doesn't have follow the
    // pelvis).
    const std::vector<glm::mat4>& joints = avatars->posedJoints(*owner.owner);
    const AvatarView::SkinBinding& binding = avatars->binding(skin);
    const auto pelvis = static_cast<size_t>(std::max(avatars->base().skeleton.find("mPelvis"), 0));
    // Kept from frame to frame (their capacity too); the deque keeps them
    // where they are, since draws point at them until the frame is drawn.
    if(palettesUsed_ == palettes_.size())
    {
        palettes_.emplace_back();
    }
    std::vector<glm::mat4>& palette = palettes_[palettesUsed_++];
    palette.resize(binding.joints.size());
    for(size_t j = 0; j < binding.joints.size(); j++)
    {
        const int joint = binding.joints[j];
        palette[j] = joints[joint < 0 ? pelvis : static_cast<size_t>(joint)] * binding.bind[j];
    }

    // Attachment draw order, as the official viewer's (llvoavatar.cpp):
    // by attachment point, then by when attached (local ids grow with it).
    const world::Object& attached = owner.root ? *owner.root : object;
    const uint64_t layer =
        (owner.animated ? 0u
                        : static_cast<uint64_t>(avatar::attachmentPointFromState(attached.state)))
            << 32 |
        attached.localId;

    stats_.drawn++;
    stats_.meshObjects++;
    for(size_t f = 0; f < faces->size() && f < world::kMaxFaces; f++)
    {
        const render::Mesh& mesh = (*faces)[f];
        const world::TextureFace& face = object.faces[f];
        if(!mesh || mesh.layout != render::VertexLayout::Skinned)
        {
            continue;
        }
        render::ObjectDraw draw{&mesh, root, render::BlinnPhongMaterial{}};
        const bool pbr = pbrMaterial(draw, object, f, world, textures, materials, texels);
        stats_.pbrFaces += pbr;
        if(!pbr)
        {
            if(face.color.a <= 0.0f || face.texture == kTransparentTexture)
            {
                continue;
            }
            // Bake on Mesh shows the wearer's bakes; an animated object has none.
            const auto m = material(face, textures, legacyMaterials, world,
                                    owner.animated ? nullptr : owner.owner, avatars, texels);
            if(!m)
            {
                continue;
            }
            draw.material = *m;
            uvTransform(face, draw.uvRow0, draw.uvRow1);
        }
        draw.joints = palette;
        draw.wearer = owner.owner;
        draw.layer = layer;
        draw.shadowOnly = seen == render::Visibility::ShadowOnly;
        scene.objects.push_back(draw);
        stats_.faces++;
    }
}

void ObjectView::addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
                       TextureManager& textures, MeshManager& meshes, MaterialManager& materials,
                       LegacyMaterialManager& legacyMaterials, const AvatarView* avatars,
                       float drawDistance)
{
    stats_.drawn = 0;
    stats_.faces = 0;
    palettesUsed_ = 0; // last frame's draws are done
    stats_.skipped = 0;
    stats_.meshObjects = 0;
    stats_.culled = 0;
    stats_.pbrFaces = 0;
    const glm::vec3 camera = scene.camera.position;
    const glm::vec3 forward = glm::normalize(scene.camera.target - scene.camera.position);

    for(const auto& [key, object] : world.objects())
    {
        if(object.type != world::ObjectType::Prim || !object.hasVolume)
        {
            continue;
        }
        if(object.light)
        {
            addLight(scene, world, object, originHandle, textures, avatars, drawDistance);
        }
        const uint8_t sculpt = object.sculptType & world::SculptType::Mask;
        const bool isMesh = sculpt == world::SculptType::Mesh;
        if(sculpt != world::SculptType::None && !isMesh)
        {
            stats_.skipped++;
            continue;
        }

        // Rigged mesh: skinned to the skeleton it follows (its avatar's or
        // its animated object's), wherever the prim itself is.
        if(isMesh && meshes.isRigged(object.sculptTexture).value_or(false))
        {
            addRigged(scene, world, object, originHandle, textures, meshes, materials,
                      legacyMaterials, avatars, drawDistance);
            continue;
        }

        const auto frame = frameOf(object, world, originHandle, avatars);
        if(!frame)
        {
            stats_.skipped++; // parent (or its avatar) not known yet
            continue;
        }
        const glm::vec3 position((*frame)[3]);

        // Distance and a rough "behind the camera" cull (unless its shadow
        // may still reach the view).
        const float radius = glm::length(object.scale) * 0.5f;
        const glm::vec3 toObject = position - camera;
        const float distance = glm::length(toObject);
        if(distance - radius > drawDistance ||
           (!scene.shadows.enabled && glm::dot(toObject, forward) < -radius))
        {
            continue;
        }

        const int detail = detailFor(radius, distance);
        const std::vector<render::Mesh>* faces = nullptr;
        glm::vec3 extent(0.5f); // mesh assets fit their box
        if(isMesh)
        {
            faces = meshes.get(object.sculptTexture, detail);
            stats_.meshObjects += faces != nullptr;
        }
        else
        {
            Geometry& geo = geometry(Key{object.volume, detail});
            faces = geo.ready ? &geo.faces : nullptr;
            extent = geo.extent;
        }
        if(!faces)
        {
            continue;
        }
        const render::Visibility seen =
            scene.visibility(position, glm::length(extent * object.scale));
        if(seen == render::Visibility::Hidden)
        {
            stats_.culled++;
            continue;
        }

        // Textures as big as the prim shows (the least for shadows only).
        const float texels = seen == render::Visibility::ShadowOnly
                                 ? 0.0f
                                 : textures.texelsFor(2.0f * radius, distance);
        const glm::mat4 transform = *frame * glm::scale(glm::mat4(1.0f), object.scale);
        const world::Object* wearer =
            object.parentId != 0 ? attachedAvatar(world, object) : nullptr;
        stats_.drawn++;
        for(size_t f = 0; f < faces->size() && f < world::kMaxFaces; f++)
        {
            const render::Mesh& mesh = (*faces)[f];
            const world::TextureFace& face = object.faces[f];
            if(!mesh)
            {
                continue;
            }
            render::ObjectDraw draw{&mesh, transform, render::BlinnPhongMaterial{}};
            // Planar mapping repeats the texture twice a meter (before the
            // face's own repeats).
            draw.planar = face.planarMapping();
            const float faceTexels =
                draw.planar
                    ? texels * 2.0f * std::max({object.scale.x, object.scale.y, object.scale.z})
                    : texels;
            const bool pbr = pbrMaterial(draw, object, f, world, textures, materials, faceTexels);
            stats_.pbrFaces += pbr;
            if(!pbr)
            {
                if(face.color.a <= 0.0f || face.texture == kTransparentTexture)
                {
                    continue;
                }
                const auto m =
                    material(face, textures, legacyMaterials, world, wearer, avatars, faceTexels);
                if(!m)
                {
                    continue;
                }
                draw.material = *m;
                uvTransform(face, draw.uvRow0, draw.uvRow1);
            }
            draw.shadowOnly = seen == render::Visibility::ShadowOnly;
            scene.objects.push_back(draw);
            stats_.faces++;
        }
    }

    stats_.meshes = cache_.size();
    stats_.palettes = palettesUsed_;
    stats_.pending = static_cast<size_t>(std::max(running_.load(), 0));
}

} // namespace viewer
