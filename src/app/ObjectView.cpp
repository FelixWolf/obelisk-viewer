#include "ObjectView.hpp"

#include "FaceUpload.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
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

// A face's texture transform with its object's texture animation (if any)
// standing in for what that animates (LLVOVolume::animateTextures).
void uvTransform(const world::TextureFace& face, const world::AnimatedTransform* animation,
                 glm::vec3& row0, glm::vec3& row1)
{
    glm::vec2 repeats = face.repeats;
    glm::vec2 offset = face.offset;
    float rotation = face.rotation;
    if(animation)
    {
        rotation = animation->rotates ? animation->rotation : rotation;
        repeats = animation->scales ? glm::vec2(animation->scaleS, animation->scaleT) : repeats;
        offset = animation->translates ? glm::vec2(animation->offsetS, animation->offsetT) : offset;
    }
    uvTransform(repeats, offset, rotation, row0, row1);
}

// An object's texture animation where it is now; nothing if it has none.
std::optional<world::AnimatedTransform> textureAnimation(const world::Object& object,
                                                         std::chrono::steady_clock::time_point now)
{
    if(!object.textureAnimation)
    {
        return std::nullopt;
    }
    const float seconds =
        std::max(0.0f, std::chrono::duration<float>(now - object.textureAnimationSince).count());
    return world::animateTexture(*object.textureAnimation,
                                 object.textureAnimationFrames +
                                     seconds * object.textureAnimation->rate);
}

// Whether the animation is on face `face` of `count`: its own face, or all
// of them if it names none (or one the object hasn't).
bool animates(const world::TextureAnimation& animation, size_t face, size_t count)
{
    return animation.face < 0 || static_cast<size_t>(animation.face) >= count ||
           static_cast<size_t>(animation.face) == face;
}

// The PBR transform after the animation's (as the official viewer's PBR
// shaders apply the texture matrix first).
void animatePbr(const world::TextureFace& face, const world::AnimatedTransform& animation,
                glm::vec3& row0, glm::vec3& row1)
{
    glm::vec3 a0;
    glm::vec3 a1;
    uvTransform(face, &animation, a0, a1);
    const glm::vec3 g0 = row0;
    const glm::vec3 g1 = row1;
    row0 = glm::vec3(g0.x * a0.x + g0.y * a1.x, g0.x * a0.y + g0.y * a1.y,
                     g0.x * a0.z + g0.y * a1.z + g0.z);
    row1 = glm::vec3(g1.x * a0.x + g1.y * a1.x, g1.x * a0.y + g1.y * a1.y,
                     g1.x * a0.z + g1.y * a1.z + g1.z);
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
    for(const uint8_t b : key.sculpt.bytes)
    {
        mix(b);
    }
    mix(key.sculptType);
    return static_cast<size_t>(h);
}

ObjectView::ObjectView(JobSystem& jobs, render::Renderer& renderer, const ObjectMotion& motion)
    : jobs_(jobs), renderer_(renderer), motion_(motion), flexible_(renderer)
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
    drawCache_.clear();
    index_.clear();
    cache_.clear();
    flexible_.clear();
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

ObjectView::Geometry& ObjectView::geometry(const Key& key, const image::TextureData* sculptMap)
{
    auto [it, inserted] = cache_.try_emplace(key);
    if(inserted)
    {
        running_++;
        // The sculpt map's pixels go with the job (maps are small).
        image::MipLevel map;
        if(sculptMap && !sculptMap->levels.empty())
        {
            map = sculptMap->levels.front();
        }
        jobs_.submit(
            [this, key, map = std::move(map)]
            {
                const float detail = volume::kDetailScales[static_cast<size_t>(key.detail)];
                generated_.push(withExtent(
                    Generated{key, key.sculptType != 0
                                       ? volume::generateSculpt(
                                             key.params, detail, key.sculptType,
                                             volume::SculptMap{map.width, map.height, map.pixels})
                                       : volume::generateVolume(key.params, detail)}));
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
                     const world::Object* wearer, const AvatarView* avatars, float texels,
                     const world::AnimatedTransform* animation, bool rigged)
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
    // An animated face's maps all follow the animation.
    const glm::vec2 animated = animation && animation->scales
                                   ? glm::vec2(animation->scaleS, animation->scaleT)
                                   : face.repeats;
    auto texelsAt = [&](glm::vec2 repeats)
    { return repeatedTexels(texels, animation ? animated : repeats); };
    m.diffuse = textures.get(face.texture, texelsAt(face.repeats));
    m.diffuseLoading = !m.diffuse && !face.texture.isNull();
    m.diffuseTint = face.color;
    m.fullbright = face.fullbright();
    // Legacy faces have no highlight, as in the official viewer, unless
    // they're "shiny" (low, medium, high): then specular color, glossiness
    // and environment intensity are all 0.25, 0.5 or 0.75 (llvovolume.cpp),
    // with the exponent glossiness squared times 368 (RenderSpecularExponent).
    const float shiny = 0.25f * static_cast<float>(face.shiny());
    m.specularTint = srgbToLinear(glm::vec3(shiny));
    m.glossiness = shiny;
    m.environment = shiny;
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
        // With a specular map, the material's specular color, glossiness
        // and environment intensity replace the face's shiny ones
        // (llvovolume.cpp: only then). Exponent: glossiness squared times
        // RenderSpecularExponent (368), as the official specular table;
        // none without glossiness.
        if(!legacy->specularMap.isNull())
        {
            m.specular =
                textures.get(legacy->specularMap, texelsAt(legacy->specularTransform.scale));
            m.specularTint = legacy->glossiness > 0.0f
                                 ? srgbToLinear(glm::vec3(legacy->specularColor))
                                 : glm::vec3(0.0f);
            m.glossiness = legacy->glossiness;
            m.environment = legacy->environment;
        }
        alphaMode = legacy->alphaMode;
        maskCutoff = legacy->alphaCutoff;
    }
    // What the diffuse alpha does, with what the texture's alpha turned out
    // to be (none until it has loaded: drawn solid meanwhile).
    const auto textureAlpha = textures.alphaMode(face.texture);
    const bool opaqueFace = face.color.a >= 1.0f;
    switch(alphaMode)
    {
    case assets::DiffuseAlphaMode::None:
        m.opaque = true;
        break;
    case assets::DiffuseAlphaMode::Blend:
        // Nothing to blend if the texture is wholly opaque (and the face
        // too): drawn solid, which looks the same and costs less.
        m.blend = !(textureAlpha == image::AlphaMode::Opaque && opaqueFace);
        break;
    case assets::DiffuseAlphaMode::Mask:
        m.alphaCutoff = maskCutoff;
        break;
    case assets::DiffuseAlphaMode::Emissive:
        m.emissiveMask = true;
        m.opaque = true;
        break;
    case assets::DiffuseAlphaMode::Default:
        // As the texture's alpha suggests: cut out if it's a mask, unless
        // the face is see-through, glows or is rigged mesh
        // (LLFace::canRenderAsMask); blended otherwise.
        if(textureAlpha == image::AlphaMode::Mask)
        {
            if(opaqueFace && face.glow <= 0.0f && !rigged)
            {
                m.alphaCutoff = 0.5f;
            }
            else
            {
                m.blend = true;
            }
        }
        m.blend = m.blend || textureAlpha == image::AlphaMode::Blend;
        break;
    }
    // A wholly transparent texture shows nothing, blended or as it says:
    // not drawn (tint alpha 0 says so).
    if(textureAlpha == image::AlphaMode::Transparent &&
       (alphaMode == assets::DiffuseAlphaMode::Blend ||
        alphaMode == assets::DiffuseAlphaMode::Default))
    {
        m.diffuseTint.a = 0.0f;
        return m;
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
                             MaterialManager& materials, float texels,
                             const world::AnimatedTransform* animation)
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
    // (After the texture animation's scale, if it has one: animatePbr.)
    const glm::vec2 animated = animation && animation->scales
                                   ? glm::vec2(animation->scaleS, animation->scaleT)
                                   : glm::vec2(1.0f);
    auto texelsOf = [&](size_t slot)
    { return repeatedTexels(texels, m.transforms[slot].scale * animated); };
    p.albedo =
        textures.get(m.textures[assets::kBaseColorTexture], texelsOf(assets::kBaseColorTexture));
    p.albedoLoading = !p.albedo && !m.textures[assets::kBaseColorTexture].isNull();
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

ObjectView::RiggedOwner ObjectView::riggedOwner(const render::Scene& scene,
                                                const world::World& world,
                                                const SkeletonOwner& owner, uint64_t originHandle,
                                                MeshManager& meshes, const AvatarView* avatars,
                                                float drawDistance) const
{
    RiggedOwner o;
    if(owner.animated)
    {
        // An animated object's skeleton stands where its root prim is
        // (llcontrolavatar.cpp matchVolumeTransform): turned by the root's
        // bind shape rotation too unless worn.
        const auto frame = frameOf(*owner.root, world, originHandle, avatars);
        if(!frame)
        {
            return o;
        }
        o.root = *frame;
        if(owner.root->parentId == 0 &&
           (owner.root->sculptType & world::SculptType::Mask) == world::SculptType::Mesh)
        {
            if(const auto rootSkin = meshes.skin(owner.root->sculptTexture))
            {
                const glm::mat3 bind(rootSkin->bindShape);
                o.root *= glm::mat4(glm::mat3_cast(glm::quat_cast(glm::mat3(
                    glm::normalize(bind[0]), glm::normalize(bind[1]), glm::normalize(bind[2])))));
            }
        }
        // Its extent is used as a radius, unlike an avatar's (LLVOVolume::calcLOD).
        o.radius = 0.5f * avatars->extent(*owner.owner);
    }
    else
    {
        o.root = avatars->rootFrame(*owner.owner, world, originHandle);
        o.radius = avatars->extent(*owner.owner);
    }
    o.known = true;
    o.distance = glm::length(glm::vec3(o.root[3]) - scene.camera.position);
    // Rigged mesh reaches past the skeleton (hair, tails, skirts): a margin.
    o.seen = o.distance > drawDistance ? render::Visibility::Hidden
                                       : scene.visibility(glm::vec3(o.root[3]), o.radius + 1.0f);
    return o;
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
    // What the skeleton's owner is to this frame (where, how big, whether
    // it's seen): the same for all of its rigged attachments, so worked out
    // once.
    const auto [ownerIt, firstOfOwner] = riggedOwners_.try_emplace(owner.owner);
    RiggedOwner& o = ownerIt->second;
    if(firstOfOwner)
    {
        o = riggedOwner(scene, world, owner, originHandle, meshes, avatars, drawDistance);
    }
    if(!o.known)
    {
        stats_.skipped++;
        return;
    }
    if(o.seen == render::Visibility::Hidden)
    {
        stats_.culled++;
        return;
    }
    const glm::mat4& root = o.root;
    const float radius = o.radius;
    const float distance = o.distance;
    const render::Visibility seen = o.seen;
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
    const auto animation = textureAnimation(object, std::chrono::steady_clock::now());
    for(size_t f = 0; f < faces->size() && f < world::kMaxFaces; f++)
    {
        const render::Mesh& mesh = (*faces)[f];
        const world::TextureFace& face = object.faces[f];
        if(!mesh || mesh.layout != render::VertexLayout::Skinned)
        {
            continue;
        }
        const world::AnimatedTransform* faceAnimation =
            animation && animates(*object.textureAnimation, f, faces->size()) ? &*animation
                                                                              : nullptr;
        render::ObjectDraw draw{&mesh, root, render::BlinnPhongMaterial{}};
        const bool pbr =
            pbrMaterial(draw, object, f, world, textures, materials, texels, faceAnimation);
        stats_.pbrFaces += pbr;
        if(pbr && faceAnimation)
        {
            animatePbr(face, *faceAnimation, draw.uvRow0, draw.uvRow1);
        }
        if(!pbr)
        {
            if(face.color.a <= 0.0f || face.texture == kTransparentTexture)
            {
                continue;
            }
            // Bake on Mesh shows the wearer's bakes; an animated object has none.
            const auto m = material(face, textures, legacyMaterials, world,
                                    owner.animated ? nullptr : owner.owner, avatars, texels,
                                    faceAnimation, true);
            if(!m || m->diffuseTint.a <= 0.0f)
            {
                continue; // a bake still loading, or nothing shows
            }
            draw.material = *m;
            uvTransform(face, faceAnimation, draw.uvRow0, draw.uvRow1);
            if(faceAnimation)
            {
                std::get<render::BlinnPhongMaterial>(draw.material).mapTransforms = false;
            }
        }
        draw.joints = palette;
        draw.wearer = owner.owner;
        draw.layer = layer;
        draw.shadowOnly = seen == render::Visibility::ShadowOnly;
        draw.glow = face.glow;
        scene.objects.push_back(draw);
        stats_.faces++;
    }
}

const world::Object& ObjectView::linkRoot(const world::Object& object, const world::World& world)
{
    const world::Object* o = &object;
    for(int depth = 0; depth < 16 && o->parentId != 0; depth++)
    {
        const world::Object* parent = world.object(o->region, o->parentId);
        if(!parent || parent->type == world::ObjectType::Avatar)
        {
            break;
        }
        o = parent;
    }
    return *o;
}

bool ObjectView::touchable(const world::Object& object, const world::Object& root)
{
    // A touch script in it or its root, unless clicking it is disabled
    // (lltoolpie.cpp).
    return object.clickAction != world::ClickAction::Disabled &&
           ((object.updateFlags & world::kFlagHandleTouch) ||
            (root.updateFlags & world::kFlagHandleTouch));
}

uint8_t ObjectView::clickAction(const world::Object& object, const world::Object& root)
{
    // Its own, else its root's; none on attachments (lltoolpie.cpp
    // useClickAction).
    if(root.parentId != 0)
    {
        return world::ClickAction::Touch; // attached to an avatar
    }
    const uint8_t own = object.clickAction;
    if(own != world::ClickAction::Touch && own != world::ClickAction::Disabled)
    {
        return own;
    }
    const uint8_t rootAction = root.clickAction;
    return rootAction != world::ClickAction::Disabled ? rootAction : world::ClickAction::Touch;
}

uint64_t ObjectView::pickRegion(uint64_t region)
{
    const auto it = std::ranges::find(pickRegions_, region);
    if(it != pickRegions_.end())
    {
        return static_cast<uint64_t>(it - pickRegions_.begin()) + 1;
    }
    pickRegions_.push_back(region);
    return pickRegions_.size();
}

std::optional<ObjectView::PickedFace> ObjectView::resolvePick(uint64_t pickId) const
{
    const uint64_t index = (pickId >> 8) & 0xFFFFFFu;
    if(pickId == 0 || index == 0 || index > pickRegions_.size())
    {
        return std::nullopt;
    }
    return PickedFace{
        world::ObjectKey{pickRegions_[index - 1], static_cast<uint32_t>(pickId >> 32)},
        static_cast<int>(pickId & 0xFFu)};
}

std::optional<glm::mat4> ObjectView::objectFrame(const world::Object& object,
                                                 const world::World& world, uint64_t originHandle,
                                                 const AvatarView* avatars) const
{
    return frameOf(object, world, originHandle, avatars);
}

bool ObjectView::onHud(const world::Object& object, const world::World& world,
                       const AvatarView* avatars) const
{
    // Up the link set to the root; attached to an avatar at a HUD point?
    const world::Object* o = &object;
    for(int depth = 0; depth < 16 && o->parentId != 0; depth++)
    {
        const world::Object* parent = world.object(o->region, o->parentId);
        if(!parent)
        {
            return false;
        }
        if(parent->type == world::ObjectType::Avatar)
        {
            return avatars && avatars->isHudPoint(avatar::attachmentPointFromState(o->state));
        }
        o = parent;
    }
    return false;
}

void ObjectView::addProbe(render::Scene& scene, const world::Object& object,
                          const glm::mat4& frame) const
{
    // A manual reflection probe: a sphere its X size across, or its box
    // (LLReflectionMap::autoAdjustOrigin, getBox).
    const world::ReflectionProbeParams& params = *object.reflectionProbe;
    render::ReflectionProbe probe;
    probe.id = world::ObjectKeyHash{}(world::ObjectKey{object.region, object.localId});
    probe.origin = glm::vec3(frame[3]);
    probe.manual = true;
    probe.ambiance = params.ambiance;
    probe.nearClip = std::max(params.clipDistance, 0.1f); // MINIMUM_NEAR_CLIP
    if(params.box())
    {
        const glm::vec3 half = object.scale * 0.5f;
        probe.radius = glm::length(half);
        probe.box = glm::inverse(frame * glm::scale(glm::mat4(1.0f), half));
    }
    else
    {
        probe.radius = object.scale.x * 0.5f;
    }
    if(probe.radius > 0.0f)
    {
        scene.probes.push_back(probe);
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
    stats_.reused = 0;
    probeObjects_.clear();
    riggedOwners_.clear();
    const glm::vec3 camera = scene.camera.position;
    const glm::vec3 forward = glm::normalize(scene.camera.target - scene.camera.position);
    const auto now = std::chrono::steady_clock::now(); // texture animations, flexible prims
    flexible_.endFrame();
    // The wind over a point in the scene: its region's (flexible prims).
    const FlexibleView::WindAt windAt = [&world, originHandle](const glm::vec3& p)
    {
        const double x = static_cast<double>(originHandle >> 32) + static_cast<double>(p.x);
        const double y = static_cast<double>(originHandle & 0xFFFFFFFFu) + static_cast<double>(p.y);
        constexpr double kWidth = world::kRegionWidth;
        const double cornerX = std::floor(x / kWidth) * kWidth;
        const double cornerY = std::floor(y / kWidth) * kWidth;
        const uint64_t handle =
            static_cast<uint64_t>(cornerX) << 32 | static_cast<uint64_t>(cornerY);
        const world::WindField* wind = world.wind(handle);
        return wind ? world::windVelocity(*wind,
                                          glm::vec2(static_cast<float>(x - cornerX),
                                                    static_cast<float>(y - cornerY)),
                                          static_cast<float>(kWidth))
                    : glm::vec3(0.0f);
    };

    // One prim (`moving`: of a link set looked at every frame).
    auto addPrim = [&](const world::ObjectKey& key, const world::Object& object, bool moving)
    {
        if(object.type != world::ObjectType::Prim || !object.hasVolume)
        {
            return;
        }
        // HUD attachments: in HUD space, drawn into the HUD only (no lights
        // or probes from them, no culling, always the most detail).
        // Its record: what it is (until it changes) and its kept draws.
        CachedDraws& record = drawCache_[key];
        record.usedFrame = frame_;
        const uint8_t sculptBits = object.sculptType & world::SculptType::Mask;
        if(!record.known || record.infoRevision != object.contentRevision ||
           (sculptBits == world::SculptType::Mesh && record.infoMeshRevision != meshes.revision()))
        {
            record.known = true;
            record.infoRevision = object.contentRevision;
            record.infoMeshRevision = meshes.revision();
            record.onHud = onHud(object, world, avatars);
            record.rigged = sculptBits == world::SculptType::Mesh &&
                            meshes.isRigged(object.sculptTexture).value_or(false);
        }
        const bool hud = record.onHud;
        const uint8_t sculpt = object.sculptType & world::SculptType::Mask;
        const bool isMesh = sculpt == world::SculptType::Mesh;
        // Sculpted prims: shaped by their sculpt map, read on the CPU; not
        // drawn until it has loaded.
        const image::TextureData* sculptMap = nullptr;
        if(sculpt != world::SculptType::None && !isMesh)
        {
            sculptMap = textures.data(object.sculptTexture);
            if(!sculptMap)
            {
                stats_.skipped++;
                return;
            }
        }

        // Rigged mesh: skinned to the skeleton it follows (its avatar's or
        // its animated object's), wherever the prim itself is.
        if(record.rigged)
        {
            if(hud)
            {
                return; // rigged mesh on a HUD: not drawn
            }
            addRigged(scene, world, object, originHandle, textures, meshes, materials,
                      legacyMaterials, avatars, drawDistance);
            return;
        }

        const auto frame = frameOf(object, world, originHandle, avatars);
        if(!frame)
        {
            stats_.skipped++; // parent (or its avatar) not known yet
            return;
        }
        const glm::vec3 position((*frame)[3]);

        // Distance and a rough "behind the camera" cull (unless its shadow
        // may still reach the view, or reflection probes see it).
        const float radius = glm::length(object.scale) * 0.5f;
        if(!hud && moving)
        {
            probeObjects_.push_back({position, radius}); // still ones: from the index
        }
        const glm::vec3 toObject = position - camera;
        const float distance = glm::length(toObject);
        const bool forProbes = scene.reflections.enabled && scene.reflections.probeFace &&
                               scene.reflections.probeFace->containsSphere(position, radius);
        if(!hud && (distance - radius > drawDistance || (!scene.shadows.enabled && !forProbes &&
                                                         glm::dot(toObject, forward) < -radius)))
        {
            return;
        }

        // Hidden (behind something, as a recent frame saw it, or out of
        // view with nothing to cast or reflect): skipped before any work.
        // The radius is generous: prims reach past their box.
        if(!hud &&
           scene.visibility(position, glm::length(object.scale)) == render::Visibility::Hidden)
        {
            stats_.culled++;
            return;
        }

        const int detail = hud ? 3 : detailFor(radius, distance);
        // Kept draws, if nothing they depend on has changed.
        const bool flexibleNow = object.flexible && sculpt == world::SculptType::None;
        const bool keepable = !flexibleNow && !object.textureAnimation;
        const void* overrides = world.materialOverrides(key);
        auto valid = [&](const CachedDraws& c)
        {
            if(c.contentRevision != object.contentRevision || c.detail != detail || c.hud != hud ||
               c.overrides != overrides || c.materialRevision != materials.revision() ||
               c.legacyRevision != legacyMaterials.revision() ||
               (c.mesh && c.meshRevision != meshes.revision()) ||
               (c.worn && c.appearanceRevision != world.appearanceRevision()))
            {
                return false;
            }
            if(c.root == key)
            {
                return true;
            }
            const world::Object* root = world.object(c.root.region, c.root.localId);
            return root && root->contentRevision == c.rootRevision;
        };
        if(keepable)
        {
            if(record.drawsValid && valid(record))
            {
                CachedDraws& c = record;
                const render::Visibility seen =
                    hud ? render::Visibility::Visible : scene.visibility(position, c.radius);
                if(seen == render::Visibility::Hidden)
                {
                    stats_.culled++;
                    return;
                }
                const float texels = hud ? 2.0f * radius * scene.hud.pixelHeight * scene.hud.zoom
                                     : seen == render::Visibility::ShadowOnly
                                         ? 0.0f
                                         : textures.texelsFor(2.0f * radius, distance);
                for(const TextureManager::TextureUse& u : c.uses)
                {
                    textures.touch(u, u.texels / c.texels * texels);
                }
                const glm::mat4 transform = *frame * glm::scale(glm::mat4(1.0f), object.scale);
                const bool shadowOnly = seen == render::Visibility::ShadowOnly;
                for(render::ObjectDraw draw : c.draws)
                {
                    draw.transform = transform;
                    draw.shadowOnly = !draw.pickOnly && shadowOnly;
                    scene.objects.push_back(draw);
                }
                stats_.drawn++;
                stats_.reused++;
                stats_.faces += c.faces;
                stats_.pbrFaces += c.pbrFaces;
                stats_.meshObjects += c.mesh;
                return;
            }
        }
        const std::vector<render::Mesh>* faces = nullptr;
        glm::vec3 extent(0.5f); // mesh assets fit their box
        // Flexible prims (not sculpted or mesh): bent each frame, their
        // geometry in meters in the prim's frame.
        const bool flexible = flexibleNow;
        if(flexible)
        {
            // The share of the view it covers: a disc of its radius, over
            // the view's height squared.
            const float across =
                radius / (std::max(distance, 0.01f) * std::tan(scene.camera.verticalFov * 0.5f));
            const float screen = hud ? 1.0f : 0.25f * glm::pi<float>() * across * across;
            faces = flexible_.faces(key, object, *frame, distance, detail, screen, now,
                                    hud ? FlexibleView::WindAt{} : windAt);
            extent = glm::vec3(1.0f); // it may bend anywhere within its length
        }
        else if(isMesh)
        {
            faces = meshes.get(object.sculptTexture, detail);
            stats_.meshObjects += faces != nullptr;
        }
        else
        {
            Geometry& geo =
                sculptMap
                    ? geometry(Key{object.volume, detail, object.sculptTexture, object.sculptType},
                               sculptMap)
                    : geometry(Key{object.volume, detail});
            faces = geo.ready ? &geo.faces : nullptr;
            extent = geo.extent;
        }
        if(!faces)
        {
            return;
        }
        const render::Visibility seen =
            hud ? render::Visibility::Visible
                : scene.visibility(position, glm::length(extent * object.scale));
        if(seen == render::Visibility::Hidden)
        {
            stats_.culled++;
            return;
        }

        // Textures as big as the prim shows (the least for shadows only; on
        // the HUD, its size on the HUD image).
        const float texels = hud ? 2.0f * radius * scene.hud.pixelHeight * scene.hud.zoom
                             : seen == render::Visibility::ShadowOnly
                                 ? 1.0f // the least (and a scale for kept draws)
                                 : textures.texelsFor(2.0f * radius, distance);
        const glm::mat4 transform =
            flexible ? *frame : *frame * glm::scale(glm::mat4(1.0f), object.scale);
        const world::Object* wearer =
            object.parentId != 0 ? attachedAvatar(world, object) : nullptr;
        stats_.drawn++;
        // Picking: unless clicks go through it, each face can be picked;
        // its invisible faces too if clicking it does something (invisible
        // buttons are common).
        const bool pickable = object.clickAction != world::ClickAction::Ignore;
        const world::Object& root = linkRoot(object, world);
        const bool actionable =
            pickable &&
            (touchable(object, root) || clickAction(object, root) != world::ClickAction::Touch);
        const uint64_t pickBase =
            pickable ? static_cast<uint64_t>(object.localId) << 32 | pickRegion(object.region) << 8
                     : 0;
        const auto animation = textureAnimation(object, now);
        // Kept for next frames if everything in them has loaded (their
        // textures' sizes then scale from this one).
        const bool keep = keepable && texels > 0.0f; // (0 only on the HUD at zoom 0)
        bool complete = true;
        const size_t firstDraw = scene.objects.size();
        const size_t facesBefore = stats_.faces;
        const size_t pbrBefore = stats_.pbrFaces;
        if(keep)
        {
            recordedUses_.clear();
            textures.record(&recordedUses_);
        }
        for(size_t f = 0; f < faces->size() && f < world::kMaxFaces; f++)
        {
            const render::Mesh& mesh = (*faces)[f];
            const world::TextureFace& face = object.faces[f];
            if(!mesh)
            {
                continue;
            }
            const world::AnimatedTransform* faceAnimation =
                animation && animates(*object.textureAnimation, f, faces->size()) ? &*animation
                                                                                  : nullptr;
            render::ObjectDraw draw{&mesh, transform, render::BlinnPhongMaterial{}};
            // Planar mapping repeats the texture twice a meter (before the
            // face's own repeats).
            draw.planar = face.planarMapping();
            const float faceTexels =
                draw.planar ? repeatedTexels(
                                  texels, glm::vec2(2.0f * std::max({object.scale.x, object.scale.y,
                                                                     object.scale.z})))
                            : texels;
            const bool pbr =
                pbrMaterial(draw, object, f, world, textures, materials, faceTexels, faceAnimation);
            stats_.pbrFaces += pbr;
            if(pbr && faceAnimation)
            {
                animatePbr(face, *faceAnimation, draw.uvRow0, draw.uvRow1);
            }
            if(!pbr)
            {
                // Nothing of it shows: transparent, or its texture wholly so.
                const bool clear = face.color.a <= 0.0f || face.texture == kTransparentTexture;
                std::optional<render::BlinnPhongMaterial> m;
                if(!clear)
                {
                    m = material(face, textures, legacyMaterials, world, wearer, avatars,
                                 faceTexels, faceAnimation);
                    if(!m)
                    {
                        complete = false; // a bake still loading
                        continue;
                    }
                }
                if(clear || m->diffuseTint.a <= 0.0f)
                {
                    if(actionable)
                    {
                        draw.pickOnly = true;
                        draw.pickId = pickBase | f;
                        draw.hud = hud;
                        uvTransform(face, faceAnimation, draw.uvRow0, draw.uvRow1);
                        scene.objects.push_back(draw);
                    }
                    continue;
                }
                draw.material = *m;
                uvTransform(face, faceAnimation, draw.uvRow0, draw.uvRow1);
                if(faceAnimation)
                {
                    // Normal and specular maps follow the animated
                    // texture (llface.cpp).
                    std::get<render::BlinnPhongMaterial>(draw.material).mapTransforms = false;
                }
            }
            draw.shadowOnly = seen == render::Visibility::ShadowOnly;
            draw.hud = hud;
            draw.glow = face.glow;
            draw.pickId = pickable ? pickBase | f : 0;
            scene.objects.push_back(draw);
            stats_.faces++;
        }
        if(keep)
        {
            textures.record(nullptr);
            complete = complete &&
                       std::ranges::all_of(recordedUses_, [](const auto& u) { return u.resident; });
            if(complete)
            {
                CachedDraws& c = record;
                c.drawsValid = true;
                c.contentRevision = object.contentRevision;
                c.root = world::ObjectKey{root.region, root.localId};
                c.rootRevision = root.contentRevision;
                c.detail = detail;
                c.hud = hud;
                c.worn = wearer != nullptr;
                c.mesh = isMesh;
                c.meshRevision = meshes.revision();
                c.materialRevision = materials.revision();
                c.legacyRevision = legacyMaterials.revision();
                c.appearanceRevision = world.appearanceRevision();
                c.overrides = overrides;
                c.radius = glm::length(extent * object.scale);
                c.texels = texels;
                c.draws.assign(scene.objects.begin() + static_cast<ptrdiff_t>(firstDraw),
                               scene.objects.end());
                c.uses = recordedUses_;
                c.faces = stats_.faces - facesBefore;
                c.pbrFaces = stats_.pbrFaces - pbrBefore;
                c.usedFrame = frame_;
            }
            else
            {
                record.drawsValid = false;
                record.draws.clear();
                record.uses.clear();
            }
        }
    };

    // Where prims are, by link set (moving ones apart), from this
    // snapshot's changes.
    index_.update(world);
    auto toScene = [&](const glm::dvec3& global)
    { return glm::vec3(global - glm::dvec3(scene.worldOrigin, 0.0)); };
    // How much of something (a sphere) needs looking at: none, only its
    // big prims (it's hidden behind something, but big things there may
    // cast a shadow into view), or all of it.
    enum class Reach : uint8_t
    {
        None,
        Big,
        All,
    };
    auto reach = [&](const glm::vec3& center, float r)
    {
        const glm::vec3 toCenter = center - camera;
        if(glm::length(toCenter) - r > drawDistance)
        {
            return Reach::None;
        }
        const bool inView = scene.frustum.containsSphere(center, r);
        const bool hidden = inView && scene.occluded(center, r);
        if(inView && !hidden)
        {
            return Reach::All;
        }
        if(scene.reflections.enabled && scene.reflections.probeFace &&
           scene.reflections.probeFace->containsSphere(center, r))
        {
            return Reach::All;
        }
        if(scene.shadows.enabled && glm::length(toCenter) - r < scene.shadows.distance + 64.0f &&
           scene.frustum.containsShadowOf(center, r, scene.shadows.towardLight))
        {
            return hidden ? Reach::Big : Reach::All;
        }
        return Reach::None;
    };
    auto addMembers = [&](std::span<const world::ObjectKey> members, bool moving)
    {
        for(const world::ObjectKey& m : members)
        {
            if(const world::Object* o = world.object(m.region, m.localId);
               o && o->type == world::ObjectType::Prim && o->hasVolume)
            {
                addPrim(m, *o, moving);
            }
        }
    };

    // Lights and reflection probes matter even unseen.
    for(const world::ObjectKey& key : index_.lights())
    {
        const world::Object* o = world.object(key.region, key.localId);
        if(o && o->light && !onHud(*o, world, avatars))
        {
            addLight(scene, world, *o, originHandle, textures, avatars, drawDistance);
        }
    }
    for(const world::ObjectKey& key : index_.probes())
    {
        const world::Object* o = world.object(key.region, key.localId);
        if(o && o->reflectionProbe && !onHud(*o, world, avatars))
        {
            if(const auto frame = frameOf(*o, world, originHandle, avatars))
            {
                addProbe(scene, *o, *frame);
            }
        }
    }
    // Moving link sets: every member, every frame.
    for(const world::ObjectKey& root : index_.moving())
    {
        const ObjectIndex::LinkSet* set = index_.linkSet(root);
        if(!set)
        {
            continue;
        }
        // Worn (not on the HUD): skipped with its avatar, if that's hidden.
        const world::Object* r = world.object(root.region, root.localId);
        const world::Object* avatar =
            r && r->parentId != 0 ? world.object(root.region, r->parentId) : nullptr;
        if(avatar && avatars && avatar->type == world::ObjectType::Avatar &&
           !avatars->isHudPoint(avatar::attachmentPointFromState(r->state)))
        {
            const glm::vec3 at(avatars->rootFrame(*avatar, world, originHandle)[3]);
            // Attachments reach past the body: a margin.
            if(reach(at, std::max(avatars->extent(*avatar), 1.5f) + 2.0f) == Reach::None)
            {
                stats_.culled += set->members.size();
                continue;
            }
        }
        addMembers(set->members, true);
    }
    // Still ones: a cell at a time, then a link set at a time.
    for(const auto& [cellKey, cell] : index_.cells())
    {
        const glm::dvec3 middle = (cell.lo + cell.hi) * 0.5;
        const float cellRadius = static_cast<float>(glm::length(cell.hi - cell.lo) * 0.5);
        const Reach cellReach = reach(toScene(middle), cellRadius);
        for(const world::ObjectKey& root : cell.roots)
        {
            const ObjectIndex::LinkSet* set = index_.linkSet(root);
            if(!set)
            {
                continue;
            }
            // Where prims are dense (automatic reflection probes): all of them.
            for(const auto& [center, half] : set->spheres)
            {
                probeObjects_.push_back({toScene(center), half});
            }
            if(cellReach == Reach::None || (cellReach == Reach::Big && set->big.empty()))
            {
                stats_.culled += set->members.size();
                continue;
            }
            const Reach setReach = std::min(cellReach, reach(toScene(set->center), set->radius));
            if(setReach == Reach::None)
            {
                stats_.culled += set->members.size();
                continue;
            }
            addMembers(setReach == Reach::Big ? std::span(set->big) : std::span(set->members),
                       false);
        }
    }

    // Kept draws of prims not drawn for a while (gone, or far) go.
    if(frame_ % 600 == 0)
    {
        std::erase_if(drawCache_,
                      [&](const auto& entry) { return frame_ - entry.second.usedFrame > 600; });
    }
    frame_++;

    stats_.meshes = cache_.size();
    stats_.palettes = palettesUsed_;
    stats_.pending = static_cast<size_t>(std::max(running_.load(), 0));
}

} // namespace viewer
