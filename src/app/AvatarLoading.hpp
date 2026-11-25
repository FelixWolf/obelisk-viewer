#pragma once

// When an avatar has loaded enough to be drawn, as the official viewer
// decides it (llvoavatar.cpp updateIsFullyLoaded, processFullyLoadedChange);
// until then it's a cloud of particles (idleUpdateLoadingEffect). Once
// loaded, it stays so: changing clothes doesn't make it a cloud again.

#include "viewer/core/Uuid.hpp"
#include "viewer/world/ParticleSystem.hpp"
#include "viewer/world/World.hpp"

#include <cstddef>
#include <optional>

namespace viewer
{

// The particles' texture: not an asset, but the official viewer's
// cloud-particle.j2c, embedded (the id is ours).
extern const Uuid kCloudTexture;

// The cloud an avatar that hasn't loaded is drawn as.
const world::ParticleSystem& cloudParticles();

// Whether an avatar bake's id is a bake at all (isTextureDefined). The
// invisible texture is one: it shows nothing, but is what was asked for.
bool bakeDefined(const Uuid& texture);

// Whether there's too little to draw the avatar from (getHasMissingParts):
// no shape, or no head, upper or lower body bake.
bool hasMissingParts(const world::AvatarAppearance* appearance);

class AvatarLoading
{
public:
    static constexpr double kSettle = 3.0;          // FIRST_APPEARANCE_CLOUD_MIN_DELAY
    static constexpr double kBakeWait = 60.0;       // MAX_TEXTURE_WAIT_TIME_SEC
    static constexpr double kAttachmentWait = 60.0; // MAX_ATTACHMENT_WAIT_TIME_SEC

    struct Inputs
    {
        bool missingParts = true;
        uint64_t bakes = 0;         // which bakes: the wait for them starts over with new ones
        bool bakesPending = false;  // some still loading
        bool meshesPending = false; // some of its attachments' still loading
        size_t attachments = 0;     // known to be worn
        // How many the simulator says it wears; nothing for our own avatar,
        // which isn't waited for that way.
        std::optional<size_t> expected;
    };

    // Each frame until loaded (`now` in seconds): whether it has loaded.
    bool update(const Inputs& in, double now);
    bool loaded() const { return loaded_; }

private:
    bool waitingForAttachments(const Inputs& in, double now);

    bool loaded_ = false;
    bool started_ = false;
    double settledAt_ = 0.0; // loading until then
    uint64_t bakes_ = 0;
    double bakesAt_ = 0.0;
    size_t expected_ = 0;
    std::optional<size_t> attachments_; // mLastCloudAttachmentCount
    double attachmentsAt_ = 0.0;        // when that last changed
};

} // namespace viewer
