#pragma once

// A VR headset, through OpenXR (SDL's GPU OpenXR support: SDL makes the
// OpenXR instance with the GPU device, on the GPU the headset is plugged
// into, and the session and swapchains for it).
//
// Use: prepare() before the GPU device, its device properties added to the
// device's (RendererOptions); start() once the device is made. Then, each
// frame: poll(); while running(), beginFrame() (waits for the headset's
// next frame: the scene is built for the poses it predicts), then
// acquireImages(), the eyes drawn into them and submitted, then endFrame().
// Frames end on a thread of the headset's own (the runtime waits there for
// the GPU to finish the eyes), so the next frame is built meanwhile.
//
// Poses are in the viewer's axes (+X forward, +Y left, +Z up, meters),
// relative to where the head was and the way it faced when last recentered
// (when the session gets focus, as the headset is put on, and recenter()),
// not to the room: the app places that origin (at the avatar's eyes). The
// head can lean 35 cm from it sideways; past that, the origin follows.

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_properties.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <memory>
#include <optional>

namespace viewer::xr
{

struct Pose
{
    glm::vec3 position{0.0f};
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct HeadsetFrame
{
    // False: the headset shows nothing of ours this frame (not drawn, but
    // endFrame still called). Set by beginFrame, cleared by acquireImages
    // if there are none.
    bool render = false;
    Pose head;
    // How high the wearer's eyes are standing (the highest above the floor
    // they've been); nothing if the runtime doesn't know where the floor is.
    std::optional<float> eyeHeight;
    struct Eye
    {
        Pose pose;
        // The eye's view: its edges as the tangents of their angles from
        // its axis (left, right, down, up; left and down negative), as
        // render::Camera::edges.
        glm::vec4 tangents{-1.0f, 1.0f, -1.0f, 1.0f};
    };
    std::array<Eye, 2> eyes; // left, right
    // The controllers (left, right), where they'll be when the frame is
    // shown; only while the session has input focus.
    struct Hand
    {
        bool tracked = false;  // grip and aim located
        Pose grip;             // where the hand holds it
        Pose aim;              // pointing: along +X
        float trigger = 0.0f;  // 0..1
        float squeeze = 0.0f;  // 0..1 (the grip)
        glm::vec2 stick{0.0f}; // right, forward: -1..1 (a Vive trackpad's touch)
        bool stickClick = false;
        bool primary = false;   // A or X
        bool secondary = false; // B or Y (a Vive's menu button)
    };
    std::array<Hand, 2> hands;
    // The images to draw each eye into (acquireImages), all this size, of
    // this format.
    std::array<SDL_GPUTexture*, 2> targets{};
    uint32_t width = 0;
    uint32_t height = 0;
    SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
};

class Headset
{
public:
    // `overscan`: each eye drawn wider than the display by this fraction of
    // its view on every side (0 to 0.5), so the runtime's reprojection of a
    // late frame has image to show at the edges when the head turns.
    static std::unique_ptr<Headset> prepare(float overscan);
    ~Headset();

    Headset(const Headset&) = delete;
    Headset& operator=(const Headset&) = delete;

    // Asks the GPU device being made for one that drives the headset.
    void addDeviceProperties(SDL_PropertiesID props);
    // The session, once the device is made with those properties; false if
    // there's no headset to use.
    bool start(SDL_GPUDevice* device);

    // The runtime's news: the session starting, stopping, ending.
    void poll();
    // Showing our frames (beginFrame to call).
    bool running() const;
    // The session is over (the runtime closed it, or the headset is gone):
    // nothing more from this one.
    bool ended() const;

    // Waits for the headset's next frame, and where the eyes will be when
    // it's shown; nothing if it isn't running. Always followed by
    // endFrame().
    std::optional<HeadsetFrame> beginFrame();
    // The frame's images to draw into (once the last frame has ended);
    // false (frame.render cleared) if there are none.
    bool acquireImages(HeadsetFrame& frame);
    // After the frame's GPU work is submitted: hands it to the headset.
    void endFrame();

    // The head's place and heading now as the origin, from the next frame.
    void recenter();

private:
    Headset();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace viewer::xr
