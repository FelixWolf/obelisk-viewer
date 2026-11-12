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
void uvTransform(const world::TextureFace& face, glm::vec3& row0, glm::vec3& row1)
{
    const float c = std::cos(face.rotation);
    const float s = std::sin(face.rotation);
    const float a = c * face.repeats.x;
    const float b = s * face.repeats.x;
    const float d = -s * face.repeats.y;
    const float e = c * face.repeats.y;
    row0 = glm::vec3(a, b, -0.5f * a - 0.5f * b + face.offset.x + 0.5f);
    row1 = glm::vec3(d, e, -0.5f * d - 0.5f * e + face.offset.y + 0.5f);
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

std::optional<render::BlinnPhongMaterial> ObjectView::material(const world::TextureFace& face,
                                                               TextureManager& textures,
                                                               const world::World& world,
                                                               const world::Object* wearer,
                                                               const AvatarView* avatars)
{
    render::BlinnPhongMaterial m;
    if(wearer && avatars)
    {
        if(const auto bake = avatars->bakeFor(world, *wearer, face.texture, textures))
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
    m.diffuse = textures.get(face.texture);
    m.diffuseTint = face.color;
    m.fullbright = face.fullbright();
    // Legacy faces have no highlight, as in the official viewer. "Shiny"
    // there reflects the environment (low, medium, high); without
    // environment reflections yet, a soft broad sheen stands in.
    m.specularTint = glm::vec3(0.04f * static_cast<float>(face.shiny()));
    m.maxExponent = 16.0f;
    const auto alpha = textures.alphaMode(face.texture);
    if(alpha == image::AlphaMode::Mask)
    {
        m.alphaCutoff = 0.5f;
    }
    m.blend = face.color.a < 1.0f || alpha == image::AlphaMode::Blend;
    return m;
}

void ObjectView::addRigged(render::Scene& scene, const world::World& world,
                           const world::Object& object, uint64_t originHandle,
                           TextureManager& textures, MeshManager& meshes, const AvatarView* avatars,
                           float drawDistance)
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
    if(!scene.frustum.containsSphere(glm::vec3(root[3]), radius + 1.0f))
    {
        stats_.culled++;
        return;
    }
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
        if(!mesh || mesh.layout != render::VertexLayout::Skinned || face.color.a <= 0.0f ||
           face.texture == kTransparentTexture)
        {
            continue;
        }
        // Bake on Mesh shows the wearer's bakes; an animated object has none.
        const auto m =
            material(face, textures, world, owner.animated ? nullptr : owner.owner, avatars);
        if(!m)
        {
            continue;
        }
        render::ObjectDraw draw{&mesh, root, *m};
        uvTransform(face, draw.uvRow0, draw.uvRow1);
        draw.joints = palette;
        draw.wearer = owner.owner;
        draw.layer = layer;
        scene.objects.push_back(draw);
        stats_.faces++;
    }
}

void ObjectView::addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
                       TextureManager& textures, MeshManager& meshes, const AvatarView* avatars,
                       float drawDistance)
{
    stats_.drawn = 0;
    stats_.faces = 0;
    palettesUsed_ = 0; // last frame's draws are done
    stats_.skipped = 0;
    stats_.meshObjects = 0;
    stats_.culled = 0;
    const glm::vec3 camera = scene.camera.position;
    const glm::vec3 forward = glm::normalize(scene.camera.target - scene.camera.position);

    for(const auto& [key, object] : world.objects())
    {
        if(object.type != world::ObjectType::Prim || !object.hasVolume)
        {
            continue;
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
            addRigged(scene, world, object, originHandle, textures, meshes, avatars, drawDistance);
            continue;
        }

        const auto frame = frameOf(object, world, originHandle, avatars);
        if(!frame)
        {
            stats_.skipped++; // parent (or its avatar) not known yet
            continue;
        }
        const glm::vec3 position((*frame)[3]);

        // Distance and a rough "behind the camera" cull.
        const float radius = glm::length(object.scale) * 0.5f;
        const glm::vec3 toObject = position - camera;
        const float distance = glm::length(toObject);
        if(distance - radius > drawDistance || glm::dot(toObject, forward) < -radius)
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
        if(!scene.frustum.containsSphere(position, glm::length(extent * object.scale)))
        {
            stats_.culled++;
            continue;
        }

        const glm::mat4 transform = *frame * glm::scale(glm::mat4(1.0f), object.scale);
        const world::Object* wearer =
            object.parentId != 0 ? attachedAvatar(world, object) : nullptr;
        stats_.drawn++;
        for(size_t f = 0; f < faces->size() && f < world::kMaxFaces; f++)
        {
            const render::Mesh& mesh = (*faces)[f];
            const world::TextureFace& face = object.faces[f];
            if(!mesh || face.color.a <= 0.0f || face.texture == kTransparentTexture)
            {
                continue;
            }
            const auto m = material(face, textures, world, wearer, avatars);
            if(!m)
            {
                continue;
            }
            render::ObjectDraw draw{&mesh, transform, *m};
            uvTransform(face, draw.uvRow0, draw.uvRow1);
            scene.objects.push_back(draw);
            stats_.faces++;
        }
    }

    stats_.meshes = cache_.size();
    stats_.palettes = palettesUsed_;
    stats_.pending = static_cast<size_t>(std::max(running_.load(), 0));
}

} // namespace viewer
