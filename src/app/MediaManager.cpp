#include "MediaManager.hpp"

#include "viewer/core/Log.hpp"

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace viewer
{

namespace
{

constexpr uint32_t kMinSize = 64;
constexpr uint32_t kMaxSize = 2048;
// Unseen this many frames: hidden (not drawn, sound still plays).
constexpr uint64_t kHiddenAfter = 30;

uint32_t sideOf(int pixels)
{
    if(pixels <= 0)
    {
        return MediaManager::kDefaultSize;
    }
    return std::clamp(static_cast<uint32_t>(pixels), kMinSize, kMaxSize);
}

// A page's address for the log: without its query or fragment, where
// tokens and other private things go.
std::string forLog(const std::string& url)
{
    return url.substr(0, url.find_first_of("?#"));
}

} // namespace

float mediaRolloff(float distance)
{
    // MediaRollOffMin, MediaRollOffMax, MediaRollOffRate.
    constexpr float kMin = 40.0f, kMax = 80.0f, kRate = 0.02f;
    if(distance > kMax)
    {
        return 0.0f;
    }
    if(distance <= kMin)
    {
        return 1.0f;
    }
    const float a = 1.0f + kRate * (distance - kMin);
    return std::min(1.0f, 1.0f / (a * a));
}

MediaManager::MediaManager(render::Renderer* renderer, audio::AudioEngine* engine,
                           StartHost startHost)
    : renderer_(renderer), engine_(engine), startHost_(std::move(startHost))
{
    if(renderer_)
    {
        bgra_ = SDL_GPUTextureSupportsFormat(
            renderer_->device(), SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB, SDL_GPU_TEXTURETYPE_2D,
            SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    }
}

MediaManager::~MediaManager()
{
    for(auto& [id, tab] : tabs_)
    {
        forget(tab);
    }
    host_.reset(); // says Quit, waits for it
}

MediaStart MediaManager::startFor(const MediaEntry& entry, const std::string& url) const
{
    if(url.empty() || !entry.allows(url))
    {
        return MediaStart::Never;
    }
    static const MediaWhitelist none;
    return mediaStart(play_, entry.autoPlay, url, whitelist_ ? *whitelist_ : none);
}

void MediaManager::update(std::span<const MediaSource> sources, const MediaWhitelist& whitelist,
                          const std::string& profile, std::chrono::steady_clock::time_point now)
{
    frame_++;
    whitelist_ = &whitelist;
    takeMessages();

    if(host_ && host_->state() == media::MediaHost::State::Failed)
    {
        log::warn("media: the host stopped: {}", host_->error());
        for(auto& [id, tab] : tabs_)
        {
            forget(tab);
        }
        tabs_.clear();
        focusedTab_ = 0;
        pressedTab_ = 0;
        hoveredTab_ = 0;
        for(auto& [key, face] : faces_)
        {
            face.tab = 0;
        }
        host_.reset();
        hostReady_ = false;
        failures_++;
        retryAt_ = now + kRetryDelay;
        revision_++;
        if(failures_ >= kMaxFailures)
        {
            log::warn("media: not trying the host again (it failed {} times)", failures_);
        }
    }
    if(host_ && !hostReady_ && host_->state() == media::MediaHost::State::Ready)
    {
        hostReady_ = true;
        failures_ = 0;
    }
    if(profile != profile_)
    {
        closeAll(); // reopened as the new profile's
        profile_ = profile;
    }

    choose(sources, now);

    for(auto& [id, tab] : tabs_)
    {
        if(tab.latest)
        {
            upload(tab);
        }
        // Not drawn lately: hidden. Never before its first frame is shown,
        // though (a page hidden while it loads never draws one).
        const bool hidden = tab.shown && frame_ - tab.seenFrame > kHiddenAfter;
        if(!tab.closing && hidden != tab.hidden && host_)
        {
            tab.hidden = hidden;
            host_->send(media::Visibility{id, hidden, hidden ? 1u : kFrameRate});
        }
        updateSound(tab);
    }
    whitelist_ = nullptr;
}

void MediaManager::choose(std::span<const MediaSource> sources,
                          std::chrono::steady_clock::time_point now)
{
    entries_.clear();
    struct Want
    {
        MediaFaceKey key;
        float distance;
        bool clicked;
    };
    std::vector<Want> wanted;
    for(const MediaSource& s : sources)
    {
        if(!s.entry)
        {
            continue;
        }
        entries_[s.key] = *s.entry;
        Face& face = faces_[s.key];
        face.frame = frame_;
        face.distance = s.distance;
        face.interact = s.interact;
        const std::string& url = s.entry->url();
        if(url != face.url)
        {
            // Its media changed: whatever it was stopped by is past.
            face.url = url;
            face.autoBlocked = false;
            face.blocked = false;
            if(Tab* tab = tabOf(s.key); tab && host_)
            {
                if(s.entry->allows(url))
                {
                    host_->send(media::Navigate{tab->id, url});
                }
                else
                {
                    close(*tab);
                }
            }
        }
        const MediaStart start = startFor(*s.entry, url);
        const bool wants = !face.blocked && start != MediaStart::Never &&
                           s.distance <= kMaxDistance &&
                           (face.clicked || (start == MediaStart::Play && !face.autoBlocked));
        if(wants)
        {
            wanted.push_back({s.key, s.distance, face.clicked});
        }
    }
    // Faces no longer there are forgotten (their tabs close).
    for(auto it = faces_.begin(); it != faces_.end();)
    {
        if(it->second.frame != frame_)
        {
            if(Tab* tab = tabOf(it->first))
            {
                close(*tab);
            }
            it = faces_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Clicked ones first, then the nearest.
    std::ranges::sort(wanted, [](const Want& a, const Want& b)
                      { return a.clicked != b.clicked ? a.clicked : a.distance < b.distance; });
    if(wanted.size() > kMaxTabs)
    {
        wanted.resize(kMaxTabs);
    }
    // Close what isn't wanted now, then open what is.
    for(auto& [id, tab] : tabs_)
    {
        if(!tab.closing &&
           std::ranges::none_of(wanted, [&](const Want& w) { return w.key == tab.face; }))
        {
            close(tab);
        }
    }
    if(wanted.empty())
    {
        return;
    }
    // The host's started when something wants it (and opens once it's ready).
    if(!ensureHost(now) || !hostReady_)
    {
        return;
    }
    for(const Want& w : wanted)
    {
        Face& face = faces_[w.key];
        if(!tabOf(w.key))
        {
            open(w.key, face, entries_[w.key]);
        }
    }
}

bool MediaManager::ensureHost(std::chrono::steady_clock::time_point now)
{
    if(host_)
    {
        return true;
    }
    if(failures_ >= kMaxFailures || (retryAt_ && now < *retryAt_) || !startHost_)
    {
        return false;
    }
    retryAt_.reset();
    host_ = startHost_();
    if(!host_)
    {
        failures_++;
        retryAt_ = now + kRetryDelay;
        return false;
    }
    log::info("media: starting the host");
    return true;
}

void MediaManager::open(const MediaFaceKey& key, Face& face, const MediaEntry& entry)
{
    // The page's size: its media's (auto-scaled: up to a power of two, so
    // it fills its texture).
    uint32_t width = sideOf(entry.width);
    uint32_t height = sideOf(entry.height);
    if(entry.autoScale)
    {
        width = std::min(std::bit_ceil(width), kMaxSize);
        height = std::min(std::bit_ceil(height), kMaxSize);
    }
    auto memory =
        media::SharedMemory::create("obelisk-media", media::frameMemorySize(width, height));
    if(!memory)
    {
        log::warn("media: no shared memory for a {}x{} page", width, height);
        return;
    }
    Tab tab;
    tab.id = nextTab_++;
    tab.face = key;
    tab.width = width;
    tab.textureWidth = std::bit_ceil(width);
    tab.textureHeight = std::bit_ceil(height);
    tab.height = height;
    tab.memory = std::move(memory);
    tab.seenFrame = frame_;
    face.tab = tab.id;
    host_->send(
        media::Open{tab.id, profile_, face.url, width, height, tab.memory->name(), kFrameRate});
    log::info("media: playing {} on face {} of {}", forLog(face.url), key.face,
              key.object.toString());
    tabs_.emplace(tab.id, std::move(tab));
}

void MediaManager::close(Tab& tab)
{
    if(tab.closing)
    {
        return;
    }
    tab.closing = true;
    if(auto it = faces_.find(tab.face); it != faces_.end() && it->second.tab == tab.id)
    {
        it->second.tab = 0;
    }
    if(pressedTab_ == tab.id)
    {
        pressedTab_ = 0;
    }
    if(focusedTab_ == tab.id)
    {
        focusedTab_ = 0;
    }
    if(hoveredTab_ == tab.id)
    {
        hoveredTab_ = 0;
    }
    forget(tab);
    if(host_)
    {
        host_->send(media::Close{tab.id});
    }
    revision_++;
}

void MediaManager::closeAll()
{
    for(auto& [id, tab] : tabs_)
    {
        close(tab);
    }
}

void MediaManager::forget(Tab& tab)
{
    if(tab.voice && engine_)
    {
        engine_->stop(tab.voice);
    }
    tab.voice = 0;
    tab.audio.reset();
    if(tab.transfer && renderer_)
    {
        SDL_ReleaseGPUTransferBuffer(renderer_->device(), tab.transfer);
    }
    tab.transfer = nullptr;
    tab.texture.reset();
    if(tab.shown)
    {
        tab.shown = false;
        revision_++;
    }
    if(tab.latest && host_)
    {
        host_->send(media::Release{tab.id, tab.latest->slot});
    }
    tab.latest.reset();
}

void MediaManager::takeMessages()
{
    if(!host_)
    {
        return;
    }
    host_->take(said_);
    for(const media::Message& m : said_)
    {
        handle(m);
    }
}

void MediaManager::handle(const media::Message& message)
{
    std::visit(
        [&](const auto& m)
        {
            using T = std::decay_t<decltype(m)>;
            if constexpr(std::is_same_v<T, media::Ready>)
            {
                log::info("media: host ready ({})", m.engine);
            }
            else if constexpr(std::is_same_v<T, media::Closed>)
            {
                if(auto it = tabs_.find(m.tab); it != tabs_.end())
                {
                    if(!it->second.closing)
                    {
                        close(it->second); // by itself
                    }
                    tabs_.erase(it);
                }
            }
            else if constexpr(requires { m.tab; })
            {
                const auto it = tabs_.find(m.tab);
                if(it == tabs_.end())
                {
                    if constexpr(std::is_same_v<T, media::Frame>)
                    {
                        host_->send(media::Release{m.tab, m.slot});
                    }
                    return;
                }
                Tab& tab = it->second;
                if constexpr(std::is_same_v<T, media::Frame>)
                {
                    if(tab.closing || m.width != tab.width || m.height != tab.height ||
                       m.slot >= media::kFrameSlots)
                    {
                        host_->send(media::Release{m.tab, m.slot});
                        return;
                    }
                    if(tab.latest)
                    {
                        host_->send(media::Release{m.tab, tab.latest->slot}); // only the newest
                    }
                    tab.latest = m;
                }
                else if constexpr(std::is_same_v<T, media::Address>)
                {
                    tab.address = m.url;
                    if(tab.closing || m.url == "about:blank")
                    {
                        return;
                    }
                    // Where it went, by itself or by a click on the page.
                    const auto f = faces_.find(tab.face);
                    const auto e = entries_.find(tab.face);
                    if(f == faces_.end() || e == entries_.end())
                    {
                        return;
                    }
                    if(!e->second.allows(m.url))
                    {
                        log::info("media: {} isn't allowed by its creator; closed", forLog(m.url));
                        f->second.blocked = true;
                        close(tab);
                    }
                    else if(!f->second.clicked && startFor(e->second, m.url) != MediaStart::Play)
                    {
                        log::info("media: {} wouldn't play by itself; closed until clicked",
                                  forLog(m.url));
                        f->second.autoBlocked = true;
                        close(tab);
                    }
                }
                else if constexpr(std::is_same_v<T, media::Editing>)
                {
                    if(m.editing && !tab.editing && focusedTab_ == tab.id)
                    {
                        editingTaken_++;
                    }
                    tab.editing = m.editing;
                }
                else if constexpr(std::is_same_v<T, media::Selection>)
                {
                    tab.selection = m.text;
                }
                else if constexpr(std::is_same_v<T, media::Crashed>)
                {
                    log::warn("media: the page on face {} of {} crashed", tab.face.face,
                              tab.face.object.toString());
                    if(const auto f = faces_.find(tab.face); f != faces_.end())
                    {
                        f->second.blocked = true; // until its media changes
                    }
                    close(tab);
                }
            }
        },
        message);
}

void MediaManager::upload(Tab& tab)
{
    const media::Frame frame = *tab.latest;
    tab.latest.reset();
    const uint8_t* pixels =
        tab.memory->data() + media::frameSlotOffset(frame.slot, tab.width, tab.height);
    if(renderer_)
    {
        SDL_GPUDevice* device = renderer_->device();
        const uint32_t bytes = static_cast<uint32_t>(media::frameSlotSize(tab.width, tab.height));
        if(!tab.texture)
        {
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = bgra_ ? SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB
                                : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
            info.width = tab.textureWidth;
            info.height = tab.textureHeight;
            info.layer_count_or_depth = 1;
            info.num_levels =
                static_cast<Uint32>(std::bit_width(std::max(tab.textureWidth, tab.textureHeight)));
            info.sample_count = SDL_GPU_SAMPLECOUNT_1;
            SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
            SDL_GPUTransferBufferCreateInfo transfer{};
            transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            transfer.size = bytes;
            tab.transfer = texture ? SDL_CreateGPUTransferBuffer(device, &transfer) : nullptr;
            if(!texture || !tab.transfer)
            {
                log::warn("media: no texture for a {}x{} page: {}", tab.width, tab.height,
                          SDL_GetError());
                if(texture)
                {
                    SDL_ReleaseGPUTexture(device, texture);
                }
                host_->send(media::Release{tab.id, frame.slot});
                return;
            }
            tab.texture = render::GpuTexture(device, texture, tab.textureWidth, tab.textureHeight,
                                             info.num_levels);
            // What the page doesn't cover: black (only the page is
            // uploaded from now on).
            if(SDL_GPUCommandBuffer* clearing = SDL_AcquireGPUCommandBuffer(device))
            {
                SDL_GPUColorTargetInfo target{};
                target.texture = texture;
                target.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
                target.load_op = SDL_GPU_LOADOP_CLEAR;
                target.store_op = SDL_GPU_STOREOP_STORE;
                SDL_EndGPURenderPass(SDL_BeginGPURenderPass(clearing, &target, 1, nullptr));
                SDL_SubmitGPUCommandBuffer(clearing);
            }
        }
        // Bottom row first, as every texture of ours (Second Life's texture
        // coordinates), and swizzled if BGRA can't be sampled.
        auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device, tab.transfer, true));
        if(mapped)
        {
            const size_t row = size_t{tab.width} * 4;
            for(uint32_t y = 0; y < tab.height; y++)
            {
                const uint8_t* from = pixels + row * (tab.height - 1 - y);
                uint8_t* to = mapped + row * y;
                if(bgra_)
                {
                    std::memcpy(to, from, row);
                }
                else
                {
                    for(size_t x = 0; x < row; x += 4)
                    {
                        to[x] = from[x + 2];
                        to[x + 1] = from[x + 1];
                        to[x + 2] = from[x];
                        to[x + 3] = from[x + 3];
                    }
                }
            }
            SDL_UnmapGPUTransferBuffer(device, tab.transfer);
            SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
            if(commands)
            {
                SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
                SDL_GPUTextureTransferInfo source{};
                source.transfer_buffer = tab.transfer;
                source.pixels_per_row = tab.width;
                source.rows_per_layer = tab.height;
                SDL_GPUTextureRegion destination{};
                destination.texture = tab.texture.get();
                destination.w = tab.width;
                destination.h = tab.height;
                destination.d = 1;
                // Not cycled: the rest of it (outside the page) stays.
                SDL_UploadToGPUTexture(copy, &source, &destination, false);
                SDL_EndGPUCopyPass(copy);
                if(tab.texture.levels() > 1)
                {
                    SDL_GenerateMipmapsForGPUTexture(commands, tab.texture.get());
                }
                SDL_SubmitGPUCommandBuffer(commands);
                if(!tab.shown)
                {
                    tab.shown = true;
                    revision_++;
                }
            }
        }
    }
    else if(!tab.shown)
    {
        tab.shown = true; // (tests: as good as drawn)
        revision_++;
    }
    host_->send(media::Release{tab.id, frame.slot});
}

void MediaManager::updateSound(Tab& tab)
{
    if(!engine_ || !host_ || tab.closing)
    {
        return;
    }
    const auto audio = host_->audio(tab.id);
    if(audio != tab.audio)
    {
        // Started again (or stopped): a new stream.
        if(tab.voice)
        {
            engine_->stop(tab.voice);
            tab.voice = 0;
        }
        tab.audio = audio;
    }
    const auto f = faces_.find(tab.face);
    const float gain = volume_ * (f == faces_.end() ? 0.0f : mediaRolloff(f->second.distance));
    if(tab.audio && !tab.voice)
    {
        audio::AudioEngine::PlayParams params;
        params.positional = false;
        params.gain = gain;
        tab.voice = engine_->playStream(tab.audio, params); // 0 until it can
    }
    else if(tab.voice)
    {
        engine_->setGain(tab.voice, gain);
    }
}

MediaManager::Tab* MediaManager::tabOf(const MediaFaceKey& key)
{
    const auto f = faces_.find(key);
    if(f == faces_.end() || f->second.tab == 0)
    {
        return nullptr;
    }
    const auto t = tabs_.find(f->second.tab);
    return t == tabs_.end() || t->second.closing ? nullptr : &t->second;
}

const MediaManager::Tab* MediaManager::tabOf(const MediaFaceKey& key) const
{
    return const_cast<MediaManager*>(this)->tabOf(key);
}

std::optional<glm::ivec2> MediaManager::pixel(const Tab& tab, glm::vec2 uv) const
{
    if(!std::isfinite(uv.x) || !std::isfinite(uv.y))
    {
        return std::nullopt;
    }
    // Texture coordinates repeat; v runs up. The page is the texture's
    // bottom left (its top at v = height / textureHeight), as
    // LLViewerMediaImpl::textureCoordsToPixels; outside it, nothing.
    const float u = uv.x - std::floor(uv.x);
    const float v = uv.y - std::floor(uv.y);
    const int x = static_cast<int>(u * static_cast<float>(tab.textureWidth));
    const int y = static_cast<int>(tab.height) -
                  static_cast<int>(std::ceil(v * static_cast<float>(tab.textureHeight)));
    if(x < 0 || y < 0 || x >= static_cast<int>(tab.width) || y >= static_cast<int>(tab.height))
    {
        return std::nullopt;
    }
    return glm::ivec2(x, y);
}

bool MediaManager::press(const Uuid& object, int face, glm::vec2 uv)
{
    if(face < 0 || face > 255)
    {
        return false;
    }
    const MediaFaceKey key{object, static_cast<uint8_t>(face)};
    const auto f = faces_.find(key);
    const auto e = entries_.find(key);
    if(f == faces_.end() || e == entries_.end())
    {
        return false;
    }
    if(Tab* tab = tabOf(key))
    {
        if(!f->second.interact)
        {
            return false; // a touch, as usual
        }
        if(tab->shown && host_)
        {
            if(const auto p = pixel(*tab, uv))
            {
                host_->send(media::Mouse{tab->id, media::MouseKind::Move, p->x, p->y});
                host_->send(media::Mouse{tab->id, media::MouseKind::Down, p->x, p->y});
                pressedTab_ = tab->id;
                hoveredAt_ = *p;
                if(focusedTab_ != tab->id)
                {
                    unfocus();
                    focusedTab_ = tab->id;
                    host_->send(media::Focus{tab->id, true});
                    if(tab->editing)
                    {
                        editingTaken_++; // (it may have had one before)
                    }
                }
            }
        }
        return true; // (still opening: the press is its)
    }
    if(startFor(e->second, f->second.url) == MediaStart::Never || f->second.blocked)
    {
        return false;
    }
    f->second.clicked = true;
    f->second.autoBlocked = false;
    log::info("media: face {} of {} clicked to play", key.face, key.object.toString());
    return true;
}

void MediaManager::release()
{
    if(pressedTab_ && host_)
    {
        if(const auto it = tabs_.find(pressedTab_); it != tabs_.end() && !it->second.closing)
        {
            host_->send(
                media::Mouse{pressedTab_, media::MouseKind::Up, hoveredAt_.x, hoveredAt_.y});
        }
    }
    pressedTab_ = 0;
}

void MediaManager::hover(const std::optional<MediaFaceKey>& face, glm::vec2 uv)
{
    Tab* tab = face ? tabOf(*face) : nullptr;
    if(tab && (!tab->shown || !faces_[*face].interact))
    {
        tab = nullptr;
    }
    // Dragging off the page still drags (as a browser's window does).
    if(!tab && pressedTab_)
    {
        return;
    }
    const media::TabId id = tab ? tab->id : 0;
    if(hoveredTab_ && hoveredTab_ != id && host_)
    {
        host_->send(media::Mouse{hoveredTab_, media::MouseKind::Leave, hoveredAt_.x, hoveredAt_.y});
        hoveredAt_ = glm::ivec2(-1);
    }
    hoveredTab_ = id;
    if(tab && host_)
    {
        if(const auto p = pixel(*tab, uv); p && *p != hoveredAt_)
        {
            hoveredAt_ = *p;
            host_->send(media::Mouse{tab->id, media::MouseKind::Move, p->x, p->y});
        }
    }
}

bool MediaManager::wheel(int dx, int dy)
{
    if(!hoveredTab_ || !host_ || (dx == 0 && dy == 0))
    {
        return false;
    }
    host_->send(media::Wheel{hoveredTab_, hoveredAt_.x, hoveredAt_.y, dx, dy});
    return true;
}

std::optional<MediaFaceKey> MediaManager::focusedFace() const
{
    const auto it = tabs_.find(focusedTab_);
    return it == tabs_.end() || it->second.closing ? std::nullopt : std::optional(it->second.face);
}

void MediaManager::unfocus()
{
    if(focusedTab_ && host_)
    {
        host_->send(media::Focus{focusedTab_, false});
    }
    focusedTab_ = 0;
}

bool MediaManager::key(media::Key key)
{
    if(!focusedTab_ || !host_)
    {
        return false;
    }
    key.tab = focusedTab_;
    host_->send(key);
    return true;
}

bool MediaManager::text(const std::string& text)
{
    if(!focusedTab_ || !host_)
    {
        return false;
    }
    host_->send(media::Text{focusedTab_, text});
    return true;
}

bool MediaManager::editing() const
{
    const auto it = tabs_.find(focusedTab_);
    return it != tabs_.end() && it->second.editing;
}

std::string MediaManager::selection() const
{
    const auto it = tabs_.find(focusedTab_);
    return it == tabs_.end() ? std::string() : it->second.selection;
}

MediaFaceState MediaManager::state(const MediaFaceKey& key) const
{
    const auto f = faces_.find(key);
    const auto e = entries_.find(key);
    if(f == faces_.end() || e == entries_.end())
    {
        return MediaFaceState::None;
    }
    if(const Tab* tab = tabOf(key))
    {
        return tab->shown ? MediaFaceState::Playing : MediaFaceState::Starting;
    }
    if(f->second.blocked)
    {
        return MediaFaceState::Blocked;
    }
    if(play_ == MediaPlay::Disable)
    {
        return MediaFaceState::Off;
    }
    if(failures_ >= kMaxFailures || (!host_ && !startHost_))
    {
        return MediaFaceState::Unavailable;
    }
    if(f->second.distance > kMaxDistance || f->second.url.empty() ||
       !e->second.allows(f->second.url))
    {
        return MediaFaceState::Off;
    }
    return f->second.clicked ? MediaFaceState::Starting : MediaFaceState::Click;
}

std::string MediaManager::address(const MediaFaceKey& key) const
{
    const Tab* tab = tabOf(key);
    return tab ? tab->address : std::string();
}

const render::GpuTexture* MediaManager::texture(const Uuid& object, size_t face)
{
    if(face > 255 || tabs_.empty())
    {
        return nullptr;
    }
    Tab* tab = tabOf({object, static_cast<uint8_t>(face)});
    if(!tab)
    {
        return nullptr;
    }
    tab->seenFrame = frame_;
    return tab->shown && tab->texture ? &tab->texture : nullptr;
}

bool MediaManager::playing(const Uuid& object, size_t face) const
{
    return face <= 255 && !tabs_.empty() && tabOf({object, static_cast<uint8_t>(face)});
}

void MediaManager::drawn(const Uuid& object)
{
    for(auto& [id, tab] : tabs_)
    {
        if(tab.face.object == object)
        {
            tab.seenFrame = frame_;
        }
    }
}

} // namespace viewer
