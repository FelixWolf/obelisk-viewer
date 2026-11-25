#include "AvatarLoading.hpp"

namespace viewer
{

const Uuid kCloudTexture = *Uuid::parse("6f0f1a52-93c5-4d3e-8b0e-2c1d7a94e6b8");

namespace
{

// IMG_DEFAULT_AVATAR and IMG_DEFAULT (indra_constants.cpp).
const Uuid kDefaultAvatarTexture = *Uuid::parse("c228d1cf-4b5d-4ba8-84f4-899a0796aa97");
const Uuid kDefaultTexture = *Uuid::parse("d2114404-dd59-4a4d-8e6c-49359e91bbf0");

} // namespace

const world::ParticleSystem& cloudParticles()
{
    static const world::ParticleSystem cloud = []
    {
        using PS = world::ParticleSystem;
        PS s;
        s.pattern = PS::kAngleCone;
        s.innerAngle = 3.14159265f;
        s.outerAngle = 0.0f;
        s.burstRate = 0.02f;
        s.burstRadius = 0.0f;
        s.burstCount = 1;
        s.burstSpeedMin = 0.1f;
        s.burstSpeedMax = 1.0f;
        s.texture = kCloudTexture;
        s.particleFlags =
            PS::kInterpolateColor | PS::kInterpolateScale | PS::kEmissive | PS::kTarget;
        s.particleMaxAge = 4.0f;
        s.startColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.5f);
        s.endColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
        s.startScale = glm::vec2(0.8f, 1.0f);
        s.endScale = glm::vec2(0.02f);
        return s;
    }();
    return cloud;
}

bool bakeDefined(const Uuid& texture)
{
    return !texture.isNull() && texture != kDefaultAvatarTexture && texture != kDefaultTexture;
}

bool hasMissingParts(const world::AvatarAppearance* appearance)
{
    if(!appearance || appearance->visualParams.size() <= 1)
    {
        return true;
    }
    for(const size_t slot : {8, 9, 10}) // head, upper, lower
    {
        if(!bakeDefined(appearance->faces[slot].texture))
        {
            return true;
        }
    }
    return false;
}

bool AvatarLoading::waitingForAttachments(const Inputs& in, double now)
{
    if(!in.expected)
    {
        return false;
    }
    if(*in.expected != expected_)
    {
        // Told of more or fewer (parseAppearanceMessage): all over again.
        expected_ = *in.expected;
        attachments_ = 0;
        attachmentsAt_ = now;
        settledAt_ = now;
    }
    if(expected_ == 0 || (attachments_ && *attachments_ >= expected_))
    {
        return false;
    }
    if(attachments_ != in.attachments)
    {
        attachments_ = in.attachments;
        attachmentsAt_ = now;
        return in.attachments != expected_;
    }
    return now - attachmentsAt_ < kAttachmentWait;
}

bool AvatarLoading::update(const Inputs& in, double now)
{
    if(loaded_)
    {
        return true;
    }
    if(!started_)
    {
        started_ = true;
        settledAt_ = now;
        bakesAt_ = now;
    }
    if(in.bakes != bakes_)
    {
        bakes_ = in.bakes;
        bakesAt_ = now;
    }
    const bool attachments = waitingForAttachments(in, now);
    const bool bakes = in.bakesPending && now - bakesAt_ < kBakeWait;
    if(in.missingParts || bakes || in.meshesPending || attachments)
    {
        settledAt_ = now;
    }
    // A little longer, for things to settle.
    loaded_ = now - settledAt_ > kSettle;
    return loaded_;
}

} // namespace viewer
