#include "viewer/xr/Headset.hpp"

#include "viewer/core/Log.hpp"

// OpenXR's own types first: SDL_openxr.h then uses them.
#include <openxr/openxr.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_openxr.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace viewer::xr
{

namespace
{

// An OpenXR structure, zeroed, of its type.
template <typename T>
T make(XrStructureType type)
{
    T value{};
    value.type = type;
    return value;
}

// OpenXR's axes (+X right, +Y up, -Z forward) to ours (+X forward, +Y
// left, +Z up): a rotation, so orientations turn the same way.
glm::vec3 toViewer(const XrVector3f& v)
{
    return {-v.z, -v.x, v.y};
}

glm::quat toViewer(const XrQuaternionf& q)
{
    return glm::quat(q.w, -q.z, -q.x, q.y);
}

const char* stateName(XrSessionState state)
{
    switch(state)
    {
    case XR_SESSION_STATE_IDLE:
        return "idle";
    case XR_SESSION_STATE_READY:
        return "ready";
    case XR_SESSION_STATE_SYNCHRONIZED:
        return "synchronized";
    case XR_SESSION_STATE_VISIBLE:
        return "visible";
    case XR_SESSION_STATE_FOCUSED:
        return "focused";
    case XR_SESSION_STATE_STOPPING:
        return "stopping";
    case XR_SESSION_STATE_LOSS_PENDING:
        return "loss pending";
    case XR_SESSION_STATE_EXITING:
        return "exiting";
    default:
        return "unknown";
    }
}

} // namespace

struct Headset::Impl
{
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    SDL_GPUDevice* device = nullptr;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE; // the room, around where the head started
    XrSpace view = XR_NULL_HANDLE;  // the head
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    bool ended = false;

    // The OpenXR functions used, from the runtime.
    PFN_xrGetInstanceProcAddr getProcAddr = nullptr;
    PFN_xrGetInstanceProperties getInstanceProperties = nullptr;
    PFN_xrGetSystemProperties getSystemProperties = nullptr;
    PFN_xrEnumerateViewConfigurationViews enumerateViewConfigurationViews = nullptr;
    PFN_xrCreateReferenceSpace createReferenceSpace = nullptr;
    PFN_xrDestroySpace destroySpace = nullptr;
    PFN_xrDestroySession destroySession = nullptr;
    PFN_xrPollEvent pollEvent = nullptr;
    PFN_xrBeginSession beginSession = nullptr;
    PFN_xrEndSession endSession = nullptr;
    PFN_xrWaitFrame waitFrame = nullptr;
    PFN_xrBeginFrame beginFrame = nullptr;
    PFN_xrEndFrame endFrame = nullptr;
    PFN_xrLocateViews locateViews = nullptr;
    PFN_xrLocateSpace locateSpace = nullptr;
    PFN_xrAcquireSwapchainImage acquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage waitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage releaseSwapchainImage = nullptr;

    struct Swapchain
    {
        XrSwapchain handle = XR_NULL_HANDLE;
        SDL_GPUTexture** images = nullptr;
        bool acquired = false; // an image of it, this frame
    };
    std::array<Swapchain, 2> swapchains{};
    uint32_t width = 0;
    uint32_t height = 0;
    SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;

    // The frame under way: waited for (xrWaitFrame), begun (xrBeginFrame),
    // its images acquired.
    bool waited = false;
    bool begun = false;
    bool rendering = false;
    XrTime displayTime = 0;
    std::array<XrView, 2> views{};

    // Ending frames (xrEndFrame, where the runtime waits for the GPU to be
    // done with the eyes) on a thread of its own, so the next frame's scene
    // is built meanwhile; the next xrBeginFrame waits for it (finishEnd).
    struct Ending
    {
        std::array<XrCompositionLayerProjectionView, 2> views{};
        XrCompositionLayerProjection layer{};
        const XrCompositionLayerBaseHeader* layers[1] = {};
        XrFrameEndInfo info{};
    };
    Ending ending;
    std::thread ender;
    std::mutex endMutex;
    std::condition_variable endChanged;
    bool endPending = false; // `ending` waits for the thread, or is being ended
    bool stopEnder = false;
    void endLoop();
    void finishEnd()
    {
        std::unique_lock lock(endMutex);
        endChanged.wait(lock, [&] { return !endPending; });
    }
    // xrBeginFrame for the frame waited for (once the last one has ended).
    void begin();

    // How much wider than the display each eye is drawn, per side, as a
    // fraction of its view (prepare).
    float overscan = 0.0f;

    // The origin: where the head was, and its heading (radians, around +Z).
    bool recenterPending = true;
    glm::vec3 origin{0.0f};
    float originYaw = 0.0f;

    bool load();
    bool createSwapchains();
    void releaseImages();
    Pose relative(const glm::vec3& position, const glm::quat& orientation) const
    {
        const glm::quat unturn = glm::angleAxis(-originYaw, glm::vec3(0.0f, 0.0f, 1.0f));
        return {unturn * (position - origin), unturn * orientation};
    }

    ~Impl()
    {
        if(ender.joinable())
        {
            {
                std::lock_guard lock(endMutex);
                stopEnder = true;
            }
            endChanged.notify_all();
            ender.join();
        }
        if(device)
        {
            SDL_WaitForGPUIdle(device);
        }
        for(Swapchain& s : swapchains)
        {
            if(s.handle)
            {
                SDL_DestroyGPUXRSwapchain(device, s.handle, s.images);
            }
        }
        for(XrSpace space : {view, local})
        {
            if(space && destroySpace)
            {
                destroySpace(space);
            }
        }
        if(session && destroySession)
        {
            destroySession(session);
        }
        // The instance goes with the GPU device (SDL made it).
    }
};

Headset::Headset() : impl_(std::make_unique<Impl>()) {}

Headset::~Headset() = default;

std::unique_ptr<Headset> Headset::prepare(float overscan)
{
    std::unique_ptr<Headset> headset(new Headset());
    headset->impl_->overscan = std::clamp(overscan, 0.0f, 0.5f);
    return headset;
}

void Headset::addDeviceProperties(SDL_PropertiesID props)
{
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_ENABLE_BOOLEAN, true);
    SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_INSTANCE_POINTER, &impl_->instance);
    SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_SYSTEM_ID_POINTER, &impl_->system);
    SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_APPLICATION_NAME_STRING, "Viewer");
    SDL_SetNumberProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_APPLICATION_VERSION_NUMBER, 1);
    SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_ENGINE_NAME_STRING, "Viewer");
}

bool Headset::Impl::load()
{
    getProcAddr =
        reinterpret_cast<PFN_xrGetInstanceProcAddr>(SDL_OpenXR_GetXrGetInstanceProcAddr());
    if(!getProcAddr)
    {
        log::error("OpenXR: no xrGetInstanceProcAddr: {}", SDL_GetError());
        return false;
    }
    bool ok = true;
    auto get = [&](const char* name, auto& function)
    {
        if(XR_FAILED(getProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&function))))
        {
            log::error("OpenXR: the runtime has no {}", name);
            ok = false;
        }
    };
    get("xrGetInstanceProperties", getInstanceProperties);
    get("xrGetSystemProperties", getSystemProperties);
    get("xrEnumerateViewConfigurationViews", enumerateViewConfigurationViews);
    get("xrCreateReferenceSpace", createReferenceSpace);
    get("xrDestroySpace", destroySpace);
    get("xrDestroySession", destroySession);
    get("xrPollEvent", pollEvent);
    get("xrBeginSession", beginSession);
    get("xrEndSession", endSession);
    get("xrWaitFrame", waitFrame);
    get("xrBeginFrame", beginFrame);
    get("xrEndFrame", endFrame);
    get("xrLocateViews", locateViews);
    get("xrLocateSpace", locateSpace);
    get("xrAcquireSwapchainImage", acquireSwapchainImage);
    get("xrWaitSwapchainImage", waitSwapchainImage);
    get("xrReleaseSwapchainImage", releaseSwapchainImage);
    return ok;
}

bool Headset::Impl::createSwapchains()
{
    uint32_t count = 0;
    if(XR_FAILED(enumerateViewConfigurationViews(
           instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr)) ||
       count < 2)
    {
        log::error("OpenXR: the headset has no stereo views");
        return false;
    }
    std::vector<XrViewConfigurationView> configs(
        count, make<XrViewConfigurationView>(XR_TYPE_VIEW_CONFIGURATION_VIEW));
    if(XR_FAILED(enumerateViewConfigurationViews(instance, system,
                                                 XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count,
                                                 &count, configs.data())))
    {
        return false;
    }
    // Both eyes at one size (the renderer draws a frame's views alike): the
    // larger of what's recommended, bigger by the overscan (as many pixels
    // to the degree).
    width = std::max(configs[0].recommendedImageRectWidth, configs[1].recommendedImageRectWidth);
    height = std::max(configs[0].recommendedImageRectHeight, configs[1].recommendedImageRectHeight);
    const float grow = 1.0f + 2.0f * overscan;
    width = static_cast<uint32_t>(std::lround(static_cast<float>(width) * grow));
    height = static_cast<uint32_t>(std::lround(static_cast<float>(height) * grow));

    // 8-bit sRGB if there is one (the hardware encodes what we write), in
    // the runtime's order of preference; otherwise its first.
    int formatCount = 0;
    SDL_GPUTextureFormat* formats = SDL_GetGPUXRSwapchainFormats(device, session, &formatCount);
    if(!formats || formatCount == 0)
    {
        log::error("OpenXR: no swapchain formats: {}", SDL_GetError());
        SDL_free(formats);
        return false;
    }
    format = formats[0];
    for(int i = 0; i < formatCount; i++)
    {
        if(formats[i] == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB ||
           formats[i] == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB)
        {
            format = formats[i];
            break;
        }
    }
    SDL_free(formats);

    for(Swapchain& s : swapchains)
    {
        XrSwapchainCreateInfo info = make<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.sampleCount = 1;
        info.width = width;
        info.height = height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if(XR_FAILED(
               SDL_CreateGPUXRSwapchain(device, session, &info, format, &s.handle, &s.images)))
        {
            log::error("OpenXR: creating a swapchain failed: {}", SDL_GetError());
            s.handle = XR_NULL_HANDLE;
            return false;
        }
    }
    log::info("OpenXR: {}x{} per eye, format {}", width, height, static_cast<int>(format));
    return true;
}

bool Headset::start(SDL_GPUDevice* device)
{
    Impl& m = *impl_;
    m.device = device;
    if(!m.instance || !m.load())
    {
        return false;
    }
    XrInstanceProperties instanceProperties =
        make<XrInstanceProperties>(XR_TYPE_INSTANCE_PROPERTIES);
    if(XR_SUCCEEDED(m.getInstanceProperties(m.instance, &instanceProperties)))
    {
        log::info("OpenXR runtime: {} {}.{}.{}", instanceProperties.runtimeName,
                  XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
                  XR_VERSION_MINOR(instanceProperties.runtimeVersion),
                  XR_VERSION_PATCH(instanceProperties.runtimeVersion));
    }
    XrSystemProperties systemProperties = make<XrSystemProperties>(XR_TYPE_SYSTEM_PROPERTIES);
    if(XR_SUCCEEDED(m.getSystemProperties(m.instance, m.system, &systemProperties)))
    {
        log::info("OpenXR headset: {}", systemProperties.systemName);
    }

    XrSessionCreateInfo sessionInfo = make<XrSessionCreateInfo>(XR_TYPE_SESSION_CREATE_INFO);
    const XrResult created = SDL_CreateGPUXRSession(device, &sessionInfo, &m.session);
    if(XR_FAILED(created))
    {
        log::error("OpenXR: creating the session failed ({}): {}", static_cast<int>(created),
                   SDL_GetError());
        m.session = XR_NULL_HANDLE;
        return false;
    }
    XrReferenceSpaceCreateInfo spaceInfo =
        make<XrReferenceSpaceCreateInfo>(XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if(XR_FAILED(m.createReferenceSpace(m.session, &spaceInfo, &m.local)))
    {
        log::error("OpenXR: no local space");
        return false;
    }
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if(XR_FAILED(m.createReferenceSpace(m.session, &spaceInfo, &m.view)))
    {
        log::error("OpenXR: no view space");
        return false;
    }
    if(!m.createSwapchains())
    {
        return false;
    }
    m.ender = std::thread([&m] { m.endLoop(); });
    return true;
}

void Headset::Impl::endLoop()
{
    std::unique_lock lock(endMutex);
    while(true)
    {
        endChanged.wait(lock, [&] { return endPending || stopEnder; });
        if(!endPending)
        {
            return; // stopping, nothing left to end
        }
        lock.unlock();
        endFrame(session, &ending.info);
        lock.lock();
        endPending = false;
        endChanged.notify_all();
    }
}

void Headset::Impl::begin()
{
    if(begun)
    {
        return;
    }
    finishEnd();
    XrFrameBeginInfo beginInfo = make<XrFrameBeginInfo>(XR_TYPE_FRAME_BEGIN_INFO);
    beginFrame(session, &beginInfo); // a discarded frame before is no matter
    begun = true;
}

void Headset::poll()
{
    Impl& m = *impl_;
    if(!m.session)
    {
        return;
    }
    XrEventDataBuffer event = make<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
    while(m.pollEvent(m.instance, &event) == XR_SUCCESS)
    {
        if(event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
            m.state = changed.state;
            log::info("OpenXR: session {}", stateName(m.state));
            switch(m.state)
            {
            case XR_SESSION_STATE_READY:
            {
                XrSessionBeginInfo begin = make<XrSessionBeginInfo>(XR_TYPE_SESSION_BEGIN_INFO);
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                m.running = XR_SUCCEEDED(m.beginSession(m.session, &begin));
                m.recenterPending = true;
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                m.finishEnd();
                m.endSession(m.session);
                m.running = false;
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                m.running = false;
                m.ended = true;
                break;
            default:
                break;
            }
        }
        else if(event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
        {
            m.running = false;
            m.ended = true;
        }
        else if(event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
        {
            m.recenterPending = true; // the runtime recentered: so do we
        }
        event = make<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
    }
}

bool Headset::running() const
{
    return impl_->running;
}

bool Headset::ended() const
{
    return impl_->ended;
}

void Headset::recenter()
{
    impl_->recenterPending = true;
}

void Headset::Impl::releaseImages()
{
    for(Swapchain& s : swapchains)
    {
        if(s.acquired)
        {
            XrSwapchainImageReleaseInfo release =
                make<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
            releaseSwapchainImage(s.handle, &release);
            s.acquired = false;
        }
    }
}

std::optional<HeadsetFrame> Headset::beginFrame()
{
    Impl& m = *impl_;
    if(!m.running || m.waited)
    {
        return std::nullopt;
    }
    // (While the last frame may still be ending, on the other thread.)
    XrFrameWaitInfo waitInfo = make<XrFrameWaitInfo>(XR_TYPE_FRAME_WAIT_INFO);
    XrFrameState frameState = make<XrFrameState>(XR_TYPE_FRAME_STATE);
    if(XR_FAILED(m.waitFrame(m.session, &waitInfo, &frameState)))
    {
        return std::nullopt;
    }
    m.waited = true;
    m.begun = false;
    m.rendering = false;
    m.displayTime = frameState.predictedDisplayTime;

    HeadsetFrame frame;
    frame.width = m.width;
    frame.height = m.height;
    frame.format = m.format;
    if(!frameState.shouldRender)
    {
        return frame;
    }

    // Where the eyes and head will be when the frame is shown.
    XrViewLocateInfo locate = make<XrViewLocateInfo>(XR_TYPE_VIEW_LOCATE_INFO);
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = m.displayTime;
    locate.space = m.local;
    XrViewState viewState = make<XrViewState>(XR_TYPE_VIEW_STATE);
    uint32_t viewCount = 0;
    for(XrView& v : m.views)
    {
        v = make<XrView>(XR_TYPE_VIEW);
    }
    if(XR_FAILED(m.locateViews(m.session, &locate, &viewState, 2, &viewCount, m.views.data())) ||
       viewCount < 2 || !(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
    {
        return frame;
    }
    XrSpaceLocation head = make<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    if(XR_FAILED(m.locateSpace(m.view, m.local, m.displayTime, &head)) ||
       !(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
    {
        return frame;
    }
    const glm::vec3 headPosition = toViewer(head.pose.position);
    const glm::quat headOrientation = toViewer(head.pose.orientation);
    if(m.recenterPending)
    {
        const glm::vec3 forward = headOrientation * glm::vec3(1.0f, 0.0f, 0.0f);
        m.origin = headPosition;
        m.originYaw = std::atan2(forward.y, forward.x);
        m.recenterPending = false;
    }
    frame.head = m.relative(headPosition, headOrientation);
    for(size_t e = 0; e < 2; e++)
    {
        XrView& v = m.views[e];
        frame.eyes[e].pose = m.relative(toViewer(v.pose.position), toViewer(v.pose.orientation));
        // Wider by the overscan on every side (in tangents, as the image's
        // pixels are): what the view is drawn and handed back with, so the
        // runtime has more than the display shows when it reprojects a late
        // frame for a head that has turned.
        glm::vec4 t(std::tan(v.fov.angleLeft), std::tan(v.fov.angleRight),
                    std::tan(v.fov.angleDown), std::tan(v.fov.angleUp));
        const float across = (t.y - t.x) * m.overscan;
        const float up = (t.w - t.z) * m.overscan;
        t += glm::vec4(-across, across, -up, up);
        v.fov = {std::atan(t.x), std::atan(t.y), std::atan(t.w), std::atan(t.z)};
        frame.eyes[e].tangents = t;
    }

    frame.render = true;
    return frame;
}

bool Headset::acquireImages(HeadsetFrame& frame)
{
    Impl& m = *impl_;
    if(!m.waited || !frame.render)
    {
        frame.render = false;
        return false;
    }
    m.begin();
    for(size_t e = 0; e < 2; e++)
    {
        Impl::Swapchain& s = m.swapchains[e];
        XrSwapchainImageAcquireInfo acquire =
            make<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
        uint32_t index = 0;
        XrSwapchainImageWaitInfo wait =
            make<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
        wait.timeout = XR_INFINITE_DURATION;
        if(XR_FAILED(m.acquireSwapchainImage(s.handle, &acquire, &index)))
        {
            m.releaseImages();
            frame.render = false;
            return false;
        }
        s.acquired = true;
        if(XR_FAILED(m.waitSwapchainImage(s.handle, &wait)))
        {
            m.releaseImages();
            frame.render = false;
            return false;
        }
        frame.targets[e] = s.images[index];
    }
    m.rendering = true;
    return true;
}

void Headset::endFrame()
{
    Impl& m = *impl_;
    if(!m.waited)
    {
        return;
    }
    m.begin(); // a frame not drawn is still begun and ended
    m.releaseImages();
    Impl::Ending& e = m.ending; // the thread is done with the last one (begin)
    e.info = make<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);
    e.info.displayTime = m.displayTime;
    e.info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if(m.rendering)
    {
        for(size_t v = 0; v < 2; v++)
        {
            e.views[v] =
                make<XrCompositionLayerProjectionView>(XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW);
            e.views[v].pose = m.views[v].pose;
            e.views[v].fov = m.views[v].fov;
            e.views[v].subImage.swapchain = m.swapchains[v].handle;
            e.views[v].subImage.imageRect.extent = {static_cast<int32_t>(m.width),
                                                    static_cast<int32_t>(m.height)};
        }
        e.layer = make<XrCompositionLayerProjection>(XR_TYPE_COMPOSITION_LAYER_PROJECTION);
        e.layer.space = m.local;
        e.layer.viewCount = 2;
        e.layer.views = e.views.data();
        e.layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&e.layer);
        e.info.layerCount = 1;
        e.info.layers = e.layers;
    }
    {
        std::lock_guard lock(m.endMutex);
        m.endPending = true;
    }
    m.endChanged.notify_all();
    m.waited = false;
    m.begun = false;
    m.rendering = false;
}

} // namespace viewer::xr
