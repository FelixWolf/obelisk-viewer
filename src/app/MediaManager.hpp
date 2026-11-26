#pragma once

// Media on prims, played: which faces' media plays, in a tab of the media
// host each (src/media; started when first needed), its frames onto the
// face (MediaTextures, for ObjectView) and its sound through the audio
// engine, quieter with distance (as the official viewer's, from 40 m to
// silence at 80 m).
//
// What plays: the media.play setting (MediaPlay) with the face's own
// auto-play and the service's whitelist (mediaStart), or a click on the
// face (press()). Never what its creator's whitelist leaves out
// (MediaEntry::allows). A page that goes by itself somewhere it wouldn't
// have started is closed until it's clicked; one that goes outside its
// creator's whitelist is closed until its media changes. At most kMaxTabs
// at once, the nearest (clicked ones first), within kMaxDistance.
//
// Once a page plays, pressing on it presses on the page (when its media
// lets anyone interact), and the pointer moving over it moves there.
//
// All on the main thread. The renderer may be null (tests): frames are then
// only taken and given back.

#include "MediaTextures.hpp"
#include "MediaWhitelist.hpp"
#include "ObjectMedia.hpp"

#include "viewer/audio/AudioEngine.hpp"
#include "viewer/media/MediaHost.hpp"
#include "viewer/media/SharedMemory.hpp"
#include "viewer/render/Renderer.hpp"

#include <glm/glm.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer
{

struct MediaFaceKey
{
    Uuid object;
    uint8_t face = 0;
    bool operator==(const MediaFaceKey&) const = default;
};

struct MediaFaceKeyHash
{
    size_t operator()(const MediaFaceKey& k) const
    {
        return std::hash<Uuid>()(k.object) * 31 + k.face;
    }
};

// A face with media this frame.
struct MediaSource
{
    MediaFaceKey key;
    const MediaEntry* entry = nullptr; // valid during update()
    float distance = 0.0f;             // from the camera, meters
    bool interact = false;             // the pointer may use the page
};

// How a face's media is doing (the Media tab says).
enum class MediaFaceState : uint8_t
{
    None,        // no media there (or nothing known)
    Off,         // media off (the setting), or out of reach
    Click,       // waiting for a click
    Starting,    // its tab is opening
    Playing,     // shown
    Blocked,     // its creator's whitelist doesn't allow where it went
    Unavailable, // no media host
};

// Volume at a distance, 0..1 (LLViewerMediaImpl::updateVolume).
float mediaRolloff(float distance);

class MediaManager final : public MediaTextures
{
public:
    static constexpr size_t kMaxTabs = 4;
    static constexpr float kMaxDistance = 64.0f;
    static constexpr uint32_t kFrameRate = 30;
    static constexpr uint32_t kDefaultSize = 1024; // a side, when the media says none
    // After the host fails: when it's tried again, and how often at most.
    static constexpr auto kRetryDelay = std::chrono::seconds(30);
    static constexpr int kMaxFailures = 3;

    using StartHost = std::function<std::unique_ptr<media::MediaHost>()>;
    MediaManager(render::Renderer* renderer, audio::AudioEngine* engine, StartHost startHost);
    ~MediaManager() override;
    MediaManager(const MediaManager&) = delete;
    MediaManager& operator=(const MediaManager&) = delete;

    void setPlay(MediaPlay play) { play_ = play; }
    void setVolume(float volume) { volume_ = volume; }

    // Once a frame: the faces with media now (all of them: others are
    // forgotten), the service's whitelist, and whose cookies and storage
    // the pages use (an account's id; switching closes every tab).
    void update(std::span<const MediaSource> sources, const MediaWhitelist& whitelist,
                const std::string& profile,
                std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    // The pointer's button on a face, at its texture coordinates: true if
    // it's media's (played, or pressed on the page): not touched then.
    bool press(const Uuid& object, int face, glm::vec2 uv);
    void release();
    // Where the pointer is (nothing: not on a face).
    void hover(const std::optional<MediaFaceKey>& face, glm::vec2 uv);

    MediaFaceState state(const MediaFaceKey& face) const;
    // What the page shows now (its address), if it plays.
    std::string address(const MediaFaceKey& face) const;
    size_t tabs() const { return tabs_.size(); }
    bool hostRunning() const { return host_ != nullptr; }

    // MediaTextures.
    const render::GpuTexture* texture(const Uuid& object, size_t face) override;
    void drawn(const Uuid& object) override;
    uint64_t revision() const override { return revision_; }

private:
    struct Face
    {
        std::string url; // what its media says to show
        bool clicked = false;
        bool autoBlocked = false; // went somewhere not to start by itself: needs a click
        bool blocked = false;     // outside its creator's whitelist
        media::TabId tab = 0;
        float distance = 0.0f;
        bool interact = false;
        uint64_t frame = 0; // last update it was there
    };
    struct Tab
    {
        media::TabId id = 0;
        MediaFaceKey face;
        uint32_t width = 0;
        uint32_t height = 0;
        std::unique_ptr<media::SharedMemory> memory;
        std::optional<media::Frame> latest; // not uploaded yet
        bool closing = false;
        bool shown = false; // a frame is on its texture
        std::string address;
        // Drawn into: texture() asked this frame, or its object was drawn.
        uint64_t seenFrame = 0;
        bool hidden = false;
        render::GpuTexture texture;
        SDL_GPUTransferBuffer* transfer = nullptr;
        audio::AudioEngine::Voice voice = 0;
        std::shared_ptr<media::TabAudio> audio;
    };

    void takeMessages();
    void handle(const media::Message& message);
    MediaStart startFor(const MediaEntry& entry, const std::string& url) const;
    void choose(std::span<const MediaSource> sources, std::chrono::steady_clock::time_point now);
    bool ensureHost(std::chrono::steady_clock::time_point now);
    void open(const MediaFaceKey& key, Face& face, const MediaEntry& entry);
    void close(Tab& tab);
    void closeAll();
    void forget(Tab& tab); // its GPU and audio things
    void upload(Tab& tab);
    void updateSound(Tab& tab);
    Tab* tabOf(const MediaFaceKey& key);
    const Tab* tabOf(const MediaFaceKey& key) const;
    std::optional<glm::ivec2> pixel(const Tab& tab, glm::vec2 uv) const;

    render::Renderer* renderer_;
    audio::AudioEngine* engine_;
    StartHost startHost_;
    std::unique_ptr<media::MediaHost> host_;
    bool hostReady_ = false;
    int failures_ = 0;
    std::optional<std::chrono::steady_clock::time_point> retryAt_;

    MediaPlay play_ = MediaPlay::Whitelist;
    float volume_ = 1.0f;
    std::string profile_;
    const MediaWhitelist* whitelist_ = nullptr; // during update()

    std::unordered_map<MediaFaceKey, Face, MediaFaceKeyHash> faces_;
    // The entries as of the last update (press() decides by them).
    std::unordered_map<MediaFaceKey, MediaEntry, MediaFaceKeyHash> entries_;
    std::unordered_map<media::TabId, Tab> tabs_;
    media::TabId nextTab_ = 1;
    uint64_t frame_ = 0;
    uint64_t revision_ = 1;

    // The pointer.
    media::TabId pressedTab_ = 0;
    media::TabId hoveredTab_ = 0;
    glm::ivec2 hoveredAt_{-1};
    bool bgra_ = true; // the GPU samples BGRA as it is (else swizzled here)
    std::vector<media::Message> said_;
};

} // namespace viewer
