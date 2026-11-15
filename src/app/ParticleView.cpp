#include "ParticleView.hpp"

#include <algorithm>
#include <cmath>

namespace viewer
{

namespace
{

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMaxStep = 0.1f; // seconds (LLViewerPartSim::updateSimulation)
// Particles without a texture: the official viewer's default image
// (pixiesmall), from the asset server.
const Uuid kDefaultImage = *Uuid::parse("168e6813-096e-07ea-97ae-fd416826f627");

// A region handle's corner, meters (x in the high half, y in the low).
glm::vec2 cornerOf(uint64_t handle)
{
    return {static_cast<float>(handle >> 32), static_cast<float>(handle & 0xFFFFFFFFu)};
}

} // namespace

void ParticleView::source(const world::ObjectKey& key, const world::ParticleSystem& system,
                          uint32_t revision, const glm::vec3& position, const glm::quat& rotation)
{
    std::shared_ptr<Source>& s = sources_[key];
    if(!s)
    {
        s = std::make_shared<Source>();
        s->system = system;
    }
    else if(s->expired && revision != s->revision)
    {
        // Updated since it ran out: a new source (unpackParticleSource).
        s->system = system;
        s->lastUpdate = 0.0f;
        s->lastPart = 0.0f;
        s->expired = false;
    }
    else if(s->system != system)
    {
        // Changed: one that had run out starts over, as does one whose
        // life changed (LLViewerPartSourceScript::unpackPSS).
        if(s->expired || (system.maxAge > 0.0f && (system.maxAge != s->system.maxAge ||
                                                   system.startAge != s->system.startAge)))
        {
            s->lastUpdate = 0.0f;
            s->lastPart = 0.0f;
            s->expired = false;
        }
        s->system = system;
    }
    s->revision = revision;
    s->position = position;
    s->objectRotation = rotation;
    s->seen = true;
}

int ParticleView::allocate()
{
    int index = 0;
    if(!free_.empty())
    {
        index = free_.back();
        free_.pop_back();
    }
    else
    {
        index = static_cast<int>(particles_.size());
        particles_.emplace_back();
    }
    particles_[static_cast<size_t>(index)] = Particle{};
    particles_[static_cast<size_t>(index)].alive = true;
    alive_++;
    return index;
}

void ParticleView::kill(int index)
{
    Particle& p = particles_[static_cast<size_t>(index)];
    if(p.source && p.source->lastParticle == index)
    {
        p.source->lastParticle = -1;
    }
    // Close the ribbon's gap (~LLViewerPart).
    if(p.parent >= 0)
    {
        particles_[static_cast<size_t>(p.parent)].child = p.child;
    }
    if(p.child >= 0)
    {
        particles_[static_cast<size_t>(p.child)].parent = p.parent;
    }
    p = Particle{};
    free_.push_back(index);
    alive_--;
}

void ParticleView::clear()
{
    sources_.clear();
    particles_.clear();
    free_.clear();
    alive_ = 0;
    haveOrigin_ = false;
}

void ParticleView::emit(const std::shared_ptr<Source>& source, float seconds,
                        const glm::vec3& camera, float pixelsPerMeter)
{
    using PS = world::ParticleSystem;
    Source& s = *source;
    const PS& sys = s.system;
    const float oldUpdate = s.lastUpdate;
    s.lastUpdate += seconds;
    const float refRate = std::min(adaptiveRate_, 1.0f);
    float due = s.lastUpdate - s.lastPart;

    if(sys.maxAge > 0.0f && sys.startAge + s.lastUpdate + due > sys.maxAge)
    {
        s.expired = true; // outlived its max age
        return;
    }
    bool first = oldUpdate <= 0.0f;
    due = std::min(due, std::max(1.0f, 10.0f * sys.burstRate));
    // Follow the source or move straight to the target: no burst radius.
    const float radius =
        (sys.particleFlags & (PS::kFollowSource | PS::kTargetLinear)) ? 0.0f : sys.burstRadius;
    while(due > sys.burstRate || first)
    {
        first = false;
        // The emission direction turns with the angular velocity.
        const float spin = glm::length(sys.angularVelocity);
        s.rotation =
            spin != 0.0f
                ? glm::normalize(glm::angleAxis(seconds * spin, sys.angularVelocity / spin) *
                                 s.rotation)
                : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        if(alive_ > kMaxParticles)
        {
            s.lastPart = s.lastUpdate;
            break;
        }
        // Too small to see at all (under a pixel at 25 cm): not emitted.
        const glm::vec2 a = sys.startScale;
        const glm::vec2 b = sys.endScale;
        const float shortSide = std::max(std::max(std::min(a.x, a.y), std::min(b.x, b.y)),
                                         std::min((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f));
        const float visibleTo = shortSide * pixelsPerMeter;
        if(visibleTo < 0.25f)
        {
            s.lastPart = s.lastUpdate;
            break;
        }
        // Farther than they show from: fewer, or none if they'd die before
        // one could get close enough to see them for 0.2 s.
        float limited = 0.0f;
        const float beyond = glm::length(s.position - camera) - visibleTo;
        if(beyond > 0.0f)
        {
            if(beyond * refRate > sys.particleMaxAge - 0.2f)
            {
                s.lastPart = s.lastUpdate;
                break;
            }
            limited = beyond * refRate / sys.particleMaxAge;
        }

        for(int i = 0; i < sys.burstCount; i++)
        {
            if(random() < std::max(1.0f - burstRate_, limited))
            {
                continue;
            }
            const int index = allocate();
            Particle& p = particles_[static_cast<size_t>(index)];
            p.source = source;
            p.flags = sys.particleFlags;
            if((p.flags & PS::kRibbon) && s.lastParticle >= 0)
            {
                // Chained to the one before.
                particles_[static_cast<size_t>(s.lastParticle)].parent = index;
                p.child = s.lastParticle;
                p.axis = s.objectRotation * glm::vec3(0.0f, 0.0f, 1.0f);
            }
            s.lastParticle = index;
            p.maxAge = sys.particleMaxAge;
            p.startColor = p.color = sys.startColor;
            p.endColor = sys.endColor;
            p.startScale = p.scale = sys.startScale;
            p.endScale = sys.endScale;
            p.acceleration = sys.acceleration;
            p.blendSource = sys.blendSource;
            p.blendDest = sys.blendDest;
            p.startGlow = p.glow = sys.startGlow;
            p.endGlow = sys.endGlow;
            p.texture = sys.texture;
            p.position = s.position;
            const float speed =
                sys.burstSpeedMin + random() * (sys.burstSpeedMax - sys.burstSpeedMin);
            if(sys.pattern & PS::kDrop)
            {
                p.velocity = glm::vec3(0.0f);
            }
            else if(sys.pattern & PS::kExplode)
            {
                glm::vec3 d;
                float length2 = 0.0f;
                do
                {
                    d = glm::vec3(random(), random(), random()) * 2.0f - 1.0f;
                    length2 = glm::dot(d, d);
                } while(length2 > 1.0f || length2 < 0.01f);
                d = glm::normalize(d);
                p.position += radius * d;
                p.velocity = d * speed;
            }
            else if(sys.pattern & (PS::kAngle | PS::kAngleCone))
            {
                // Between the inner and outer angles from the source's +Z,
                // either way (about X); a cone turns that about Z too.
                float angle = sys.innerAngle + random() * (sys.outerAngle - sys.innerAngle);
                if(random() < 0.5f)
                {
                    angle = -angle;
                }
                glm::vec3 d = glm::angleAxis(angle, glm::vec3(1.0f, 0.0f, 0.0f)) *
                              glm::vec3(0.0f, 0.0f, 1.0f);
                if(sys.pattern & PS::kAngleCone)
                {
                    d = glm::angleAxis(random() * 4.0f * kPi, glm::vec3(0.0f, 0.0f, 1.0f)) * d;
                }
                if(!(sys.flags & PS::kNewAngles))
                {
                    // The deprecated angles: turned by the outer one again.
                    d = glm::angleAxis(sys.outerAngle, glm::vec3(1.0f, 0.0f, 0.0f)) * d;
                }
                d = s.rotation * (s.objectRotation * d);
                p.position += radius * d;
                p.velocity = d * speed;
            }
            else
            {
                p.velocity = glm::vec3(0.0f);
            }
        }
        s.lastPart = s.lastUpdate;
        due -= sys.burstRate;
    }
}

void ParticleView::update(float seconds, uint64_t originHandle, const glm::vec3& camera,
                          float pixelsPerMeter, const Locate& locate, const WindAt& windAt)
{
    using PS = world::ParticleSystem;
    seconds = std::clamp(seconds, 0.0f, kMaxStep);
    frame_++;
    time_ += seconds;

    // The scene's origin moved (another region): everything with it.
    if(haveOrigin_ && originHandle != originHandle_)
    {
        const glm::vec2 shift = cornerOf(originHandle_) - cornerOf(originHandle);
        for(Particle& p : particles_)
        {
            p.position += glm::vec3(shift, 0.0f);
        }
        for(auto& [key, s] : sources_)
        {
            s->position += glm::vec3(shift, 0.0f);
            s->target += glm::vec3(shift, 0.0f);
        }
    }
    originHandle_ = originHandle;
    haveOrigin_ = true;

    // Sources: those gone stop; the rest emit, starting at a random one so
    // that none always gets first pick near the limit.
    std::vector<std::shared_ptr<Source>> active;
    for(auto it = sources_.begin(); it != sources_.end();)
    {
        if(!it->second->seen)
        {
            it->second->lastParticle = -1;
            it = sources_.erase(it);
            continue;
        }
        it->second->seen = false;
        if(!it->second->expired)
        {
            active.push_back(it->second);
        }
        ++it;
    }
    if(!active.empty())
    {
        std::rotate(active.begin(),
                    active.begin() +
                        static_cast<ptrdiff_t>(random() * static_cast<float>(active.size() - 1)),
                    active.end());
    }
    for(const std::shared_ptr<Source>& s : active)
    {
        const std::optional<glm::vec3> target =
            s->system.target.isNull() || !locate ? std::nullopt : locate(s->system.target);
        s->target = target ? *target : s->position;
        emit(s, seconds, camera, pixelsPerMeter);
    }

    // Particles.
    for(size_t i = 0; i < particles_.size(); i++)
    {
        Particle& p = particles_[i];
        if(!p.alive)
        {
            continue;
        }
        const Source& s = *p.source;
        const float age = p.age + seconds;
        const float frac = age / p.maxAge;
        if(p.flags & PS::kFollowSource)
        {
            p.position = s.position + p.offset;
        }
        if((p.flags & PS::kWind) && windAt)
        {
            p.velocity *= 1.0f - 0.1f * seconds;
            p.velocity += 0.1f * seconds * windAt(p.position);
        }
        if(p.flags & PS::kTarget)
        {
            // Steered to reach the target as it dies.
            const float remaining = p.maxAge - p.age;
            const float step = std::clamp(seconds / remaining, 0.0f, 0.1f) * 5.0f;
            const glm::vec3 toward = (s.target - p.position) / remaining;
            p.velocity = p.velocity * (1.0f - step) + step * toward;
        }
        if(p.flags & PS::kTargetLinear)
        {
            const glm::vec3 delta = s.target - s.position;
            p.position = s.position + frac * delta;
            p.velocity = delta;
        }
        else
        {
            p.position += seconds * p.velocity + 0.5f * seconds * seconds * p.acceleration;
            p.velocity += p.acceleration * seconds;
        }
        if(p.flags & PS::kBounce)
        {
            // Off the source's height.
            const float dz = p.position.z - s.position.z;
            if(dz < 0.0f)
            {
                p.position.z -= 2.0f * dz;
                p.velocity.z *= -0.75f;
            }
        }
        if(p.flags & PS::kFollowSource)
        {
            p.offset = p.position - s.position;
        }
        if(p.flags & PS::kInterpolateColor)
        {
            p.color = p.startColor * (1.0f - frac) + p.endColor * frac;
        }
        if(p.flags & PS::kInterpolateScale)
        {
            p.scale = p.startScale * (1.0f - frac) + p.endScale * frac;
        }
        p.glow = p.startGlow + (p.endGlow - p.startGlow) * frac;
        p.age = age;
        if(p.age > p.maxAge)
        {
            kill(static_cast<int>(i));
        }
    }

    // Every 16 frames, the throttles (LLViewerPartSim::updateSimulation,
    // updatePartBurstRate).
    if(frame_ % 16 == 0)
    {
        const auto count = static_cast<float>(alive_);
        const auto max = static_cast<float>(kMaxParticles);
        if(count > max * 0.875f && adaptiveRate_ < 2.0f)
        {
            adaptiveRate_ *= 2.0f;
        }
        else if(count < max * 0.5f && adaptiveRate_ > 0.03125f)
        {
            adaptiveRate_ *= 0.5f;
        }
        if(alive_ >= kHardLimit)
        {
            burstRate_ = 0.0f;
        }
        else if(alive_ > 0)
        {
            if(burstRate_ > 0.0000001f)
            {
                const float total = count / burstRate_; // estimated, unthrottled
                const float wanted = std::clamp(0.9f * max / total, 0.0f, 1.0f);
                const float limit = std::min(0.1f * std::max(wanted, burstRate_), 0.1f);
                const float delta = std::clamp(wanted - burstRate_, -limit, limit);
                burstRate_ = std::clamp(burstRate_ + 0.5f * delta, 0.0f, 1.0f);
            }
            else
            {
                burstRate_ += 0.0000001f;
            }
        }
        else
        {
            burstRate_ += 0.00125f;
        }
    }
}

void ParticleView::addTo(render::Scene& scene, const TextureFor& textures,
                         float pixelsPerMeter) const
{
    using PS = world::ParticleSystem;
    if(alive_ == 0)
    {
        return;
    }
    // Each texture once, as big as its biggest particle shows.
    std::unordered_map<Uuid, float> sizes;
    for(const Particle& p : particles_)
    {
        if(p.alive)
        {
            const float distance = std::max(glm::length(p.position - scene.camera.position), 0.5f);
            float& texels = sizes[p.texture.isNull() ? kDefaultImage : p.texture];
            texels = std::max(texels, std::max(p.scale.x, p.scale.y) / distance * pixelsPerMeter);
        }
    }
    std::unordered_map<Uuid, const render::GpuTexture*> resolved;
    for(const auto& [id, texels] : sizes)
    {
        resolved[id] = textures(id, std::clamp(texels, 16.0f, 1024.0f));
    }

    scene.particles.reserve(scene.particles.size() + static_cast<size_t>(alive_));
    for(const Particle& p : particles_)
    {
        if(!p.alive)
        {
            continue;
        }
        // Tiny and more than 5 m away: not drawn (LLVOPartGroup).
        const glm::vec3 toCamera = p.position - scene.camera.position;
        const float distance2 = glm::dot(toCamera, toCamera);
        if(distance2 > 25.0f && p.scale.x * p.scale.y / distance2 < 0.005f * 0.005f)
        {
            continue;
        }
        render::ParticleDraw d;
        d.position = p.position;
        d.size = p.scale;
        d.color = p.color;
        d.glow = p.glow;
        // Its texture; not drawn until that's in, but the default image's
        // stand-in (the renderer's soft dot) shows meanwhile.
        const auto it = resolved.find(p.texture.isNull() ? kDefaultImage : p.texture);
        d.texture = it != resolved.end() ? it->second : nullptr;
        if(!d.texture && !p.texture.isNull())
        {
            continue;
        }
        d.blendSource = p.blendSource;
        d.blendDest = p.blendDest;
        d.fullbright = (p.flags & PS::kEmissive) != 0;
        if(p.flags & PS::kFollowVelocity)
        {
            d.velocity = p.velocity;
        }
        if(p.flags & PS::kRibbon)
        {
            // From it to the one emitted after it, or its source.
            d.ribbon = true;
            d.axis = p.axis;
            if(p.parent >= 0)
            {
                const Particle& parent = particles_[static_cast<size_t>(p.parent)];
                d.otherPosition = parent.position;
                d.otherAxis = parent.axis;
                d.otherWidth = parent.scale.x;
                d.otherColor = parent.color;
                d.otherGlow = parent.glow;
            }
            else
            {
                const Source& s = *p.source;
                d.otherPosition = s.position;
                d.otherAxis = s.objectRotation * glm::vec3(0.0f, 0.0f, 1.0f);
                d.otherWidth = p.startScale.x;
                d.otherColor = p.startColor;
                d.otherGlow = p.startGlow;
            }
        }
        scene.particles.push_back(d);
    }
}

} // namespace viewer
