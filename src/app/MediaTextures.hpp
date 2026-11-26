#pragma once

// What a face shows instead of its texture while its media plays (an
// interface, so ObjectView needn't know where pages come from).

#include "viewer/core/Uuid.hpp"
#include "viewer/render/GpuTexture.hpp"

#include <cstddef>
#include <cstdint>

namespace viewer
{

class MediaTextures
{
public:
    virtual ~MediaTextures() = default;
    // The page on this face of this object, once it has drawn (null: the
    // face's own texture). Asking says it's in view.
    virtual const render::GpuTexture* texture(const Uuid& object, size_t face) = 0;
    // Whether a page plays (or is opening) on this face: its draws are kept
    // as having media (so they keep saying they're in view) even before the
    // page has drawn.
    virtual bool playing(const Uuid& object, size_t face) const = 0;
    // An object drawn from kept draws with media in them: still in view.
    virtual void drawn(const Uuid& object) = 0;
    // Changes whenever a texture() answer may have (kept draws are made
    // again).
    virtual uint64_t revision() const = 0;
};

} // namespace viewer
