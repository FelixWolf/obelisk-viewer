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
#include <cstdio>
#include <initializer_list>
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

// How far the head can lean (horizontally) from the origin before the
// origin follows it.
constexpr float kLeash = 0.35f;

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
    PFN_xrStringToPath stringToPath = nullptr;
    PFN_xrCreateActionSet createActionSet = nullptr;
    PFN_xrCreateAction createAction = nullptr;
    PFN_xrSuggestInteractionProfileBindings suggestBindings = nullptr;
    PFN_xrAttachSessionActionSets attachActionSets = nullptr;
    PFN_xrCreateActionSpace createActionSpace = nullptr;
    PFN_xrSyncActions syncActions = nullptr;
    PFN_xrGetActionStateFloat getFloat = nullptr;
    PFN_xrGetActionStateVector2f getVector2 = nullptr;
    PFN_xrGetActionStateBoolean getBoolean = nullptr;

    // The controllers (one action each, for both hands: left, right).
    XrActionSet actions = XR_NULL_HANDLE;
    std::array<XrPath, 2> handPaths{};
    XrAction gripPose = XR_NULL_HANDLE;
    XrAction aimPose = XR_NULL_HANDLE;
    XrAction trigger = XR_NULL_HANDLE;
    XrAction squeeze = XR_NULL_HANDLE;
    XrAction stick = XR_NULL_HANDLE;
    XrAction stickClick = XR_NULL_HANDLE;
    XrAction primary = XR_NULL_HANDLE;
    XrAction secondary = XR_NULL_HANDLE;
    std::array<XrSpace, 2> gripSpaces{};
    std::array<XrSpace, 2> aimSpaces{};
    bool createActions();
    void readHands(HeadsetFrame& frame);

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
        for(XrSpace space : {gripSpaces[0], gripSpaces[1], aimSpaces[0], aimSpaces[1], view, local})
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
    get("xrStringToPath", stringToPath);
    get("xrCreateActionSet", createActionSet);
    get("xrCreateAction", createAction);
    get("xrSuggestInteractionProfileBindings", suggestBindings);
    get("xrAttachSessionActionSets", attachActionSets);
    get("xrCreateActionSpace", createActionSpace);
    get("xrSyncActions", syncActions);
    get("xrGetActionStateFloat", getFloat);
    get("xrGetActionStateVector2f", getVector2);
    get("xrGetActionStateBoolean", getBoolean);
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
    if(!m.createActions())
    {
        log::warn("OpenXR: no controller input");
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
            case XR_SESSION_STATE_FOCUSED:
                // Put on (or back on): the origin where the head is now, not
                // where the headset lay before.
                m.recenterPending = true;
                break;
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
    // A leash: the head can lean so far from the origin; past that (walking
    // round the room), the origin follows it.
    {
        const glm::vec3 from = headPosition - m.origin;
        const float away = glm::length(glm::vec2(from));
        if(away > kLeash)
        {
            m.origin += glm::vec3(glm::vec2(from) * (1.0f - kLeash / away), 0.0f);
        }
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

    m.readHands(frame);
    frame.render = true;
    return frame;
}

bool Headset::Impl::createActions()
{
    XrActionSetCreateInfo setInfo = make<XrActionSetCreateInfo>(XR_TYPE_ACTION_SET_CREATE_INFO);
    std::snprintf(setInfo.actionSetName, sizeof(setInfo.actionSetName), "viewer");
    std::snprintf(setInfo.localizedActionSetName, sizeof(setInfo.localizedActionSetName), "Viewer");
    if(XR_FAILED(createActionSet(instance, &setInfo, &actions)))
    {
        return false;
    }
    auto path = [&](const char* text)
    {
        XrPath p = XR_NULL_PATH;
        stringToPath(instance, text, &p);
        return p;
    };
    handPaths = {path("/user/hand/left"), path("/user/hand/right")};
    bool ok = true;
    auto action = [&](XrAction& out, const char* name, const char* label, XrActionType type)
    {
        XrActionCreateInfo info = make<XrActionCreateInfo>(XR_TYPE_ACTION_CREATE_INFO);
        std::snprintf(info.actionName, sizeof(info.actionName), "%s", name);
        std::snprintf(info.localizedActionName, sizeof(info.localizedActionName), "%s", label);
        info.actionType = type;
        info.countSubactionPaths = 2;
        info.subactionPaths = handPaths.data();
        ok = ok && XR_SUCCEEDED(createAction(actions, &info, &out));
    };
    action(gripPose, "grip_pose", "Hand", XR_ACTION_TYPE_POSE_INPUT);
    action(aimPose, "aim_pose", "Pointer", XR_ACTION_TYPE_POSE_INPUT);
    action(trigger, "trigger", "Touch", XR_ACTION_TYPE_FLOAT_INPUT);
    action(squeeze, "squeeze", "Grab", XR_ACTION_TYPE_FLOAT_INPUT);
    action(stick, "stick", "Move and turn", XR_ACTION_TYPE_VECTOR2F_INPUT);
    action(stickClick, "stick_click", "Stick click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    action(primary, "primary", "Primary button", XR_ACTION_TYPE_BOOLEAN_INPUT);
    action(secondary, "secondary", "Secondary button", XR_ACTION_TYPE_BOOLEAN_INPUT);
    if(!ok)
    {
        return false;
    }

    // Suggested bindings for the controllers people have; the runtime maps
    // others to one of these.
    struct Binding
    {
        XrAction action;
        const char* path; // after /user/hand/{left,right}/input/
    };
    auto suggest = [&](const char* profile, std::initializer_list<Binding> both,
                       std::initializer_list<Binding> leftOnly = {},
                       std::initializer_list<Binding> rightOnly = {})
    {
        std::vector<XrActionSuggestedBinding> list;
        auto add = [&](const char* hand, const Binding& b)
        {
            char text[256];
            std::snprintf(text, sizeof(text), "/user/hand/%s/input/%s", hand, b.path);
            list.push_back({b.action, path(text)});
        };
        for(const Binding& b : both)
        {
            add("left", b);
            add("right", b);
        }
        for(const Binding& b : leftOnly)
        {
            add("left", b);
        }
        for(const Binding& b : rightOnly)
        {
            add("right", b);
        }
        XrInteractionProfileSuggestedBinding suggested = make<XrInteractionProfileSuggestedBinding>(
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
        suggested.interactionProfile = path(profile);
        suggested.countSuggestedBindings = static_cast<uint32_t>(list.size());
        suggested.suggestedBindings = list.data();
        if(XR_FAILED(suggestBindings(instance, &suggested)))
        {
            log::warn("OpenXR: bindings for {} refused", profile);
        }
    };
    suggest("/interaction_profiles/valve/index_controller", {{gripPose, "grip/pose"},
                                                             {aimPose, "aim/pose"},
                                                             {trigger, "trigger/value"},
                                                             {squeeze, "squeeze/value"},
                                                             {stick, "thumbstick"},
                                                             {stickClick, "thumbstick/click"},
                                                             {primary, "a/click"},
                                                             {secondary, "b/click"}});
    suggest("/interaction_profiles/htc/vive_controller", {{gripPose, "grip/pose"},
                                                          {aimPose, "aim/pose"},
                                                          {trigger, "trigger/value"},
                                                          {squeeze, "squeeze/click"},
                                                          {stick, "trackpad"},
                                                          {stickClick, "trackpad/click"},
                                                          {secondary, "menu/click"}});
    suggest("/interaction_profiles/oculus/touch_controller",
            {{gripPose, "grip/pose"},
             {aimPose, "aim/pose"},
             {trigger, "trigger/value"},
             {squeeze, "squeeze/value"},
             {stick, "thumbstick"},
             {stickClick, "thumbstick/click"}},
            {{primary, "x/click"}, {secondary, "y/click"}},
            {{primary, "a/click"}, {secondary, "b/click"}});
    suggest("/interaction_profiles/khr/simple_controller", {{gripPose, "grip/pose"},
                                                            {aimPose, "aim/pose"},
                                                            {trigger, "select/click"},
                                                            {secondary, "menu/click"}});

    XrSessionActionSetsAttachInfo attach =
        make<XrSessionActionSetsAttachInfo>(XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
    attach.countActionSets = 1;
    attach.actionSets = &actions;
    if(XR_FAILED(attachActionSets(session, &attach)))
    {
        return false;
    }
    for(size_t h = 0; h < 2; h++)
    {
        XrActionSpaceCreateInfo info =
            make<XrActionSpaceCreateInfo>(XR_TYPE_ACTION_SPACE_CREATE_INFO);
        info.subactionPath = handPaths[h];
        info.poseInActionSpace.orientation.w = 1.0f;
        info.action = gripPose;
        createActionSpace(session, &info, &gripSpaces[h]);
        info.action = aimPose;
        createActionSpace(session, &info, &aimSpaces[h]);
    }
    return true;
}

void Headset::Impl::readHands(HeadsetFrame& frame)
{
    if(!actions)
    {
        return;
    }
    XrActiveActionSet active{actions, XR_NULL_PATH};
    XrActionsSyncInfo sync = make<XrActionsSyncInfo>(XR_TYPE_ACTIONS_SYNC_INFO);
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if(syncActions(session, &sync) != XR_SUCCESS)
    {
        return; // not focused: no input
    }
    for(size_t h = 0; h < 2; h++)
    {
        HeadsetFrame::Hand& hand = frame.hands[h];
        XrActionStateGetInfo get = make<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
        get.subactionPath = handPaths[h];
        auto value = [&](XrAction a)
        {
            get.action = a;
            XrActionStateFloat got = make<XrActionStateFloat>(XR_TYPE_ACTION_STATE_FLOAT);
            return XR_SUCCEEDED(getFloat(session, &get, &got)) && got.isActive ? got.currentState
                                                                               : 0.0f;
        };
        auto pressed = [&](XrAction a)
        {
            get.action = a;
            XrActionStateBoolean got = make<XrActionStateBoolean>(XR_TYPE_ACTION_STATE_BOOLEAN);
            return XR_SUCCEEDED(getBoolean(session, &get, &got)) && got.isActive &&
                   got.currentState;
        };
        hand.trigger = value(trigger);
        hand.squeeze = value(squeeze);
        get.action = stick;
        XrActionStateVector2f stickState =
            make<XrActionStateVector2f>(XR_TYPE_ACTION_STATE_VECTOR2F);
        if(XR_SUCCEEDED(getVector2(session, &get, &stickState)) && stickState.isActive)
        {
            hand.stick = glm::vec2(stickState.currentState.x, stickState.currentState.y);
        }
        hand.stickClick = pressed(stickClick);
        hand.primary = pressed(primary);
        hand.secondary = pressed(secondary);

        auto locate = [&](XrSpace space, Pose& out)
        {
            XrSpaceLocation where = make<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
            constexpr XrSpaceLocationFlags kValid =
                XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if(!space || XR_FAILED(locateSpace(space, local, displayTime, &where)) ||
               (where.locationFlags & kValid) != kValid)
            {
                return false;
            }
            out = relative(toViewer(where.pose.position), toViewer(where.pose.orientation));
            return true;
        };
        hand.tracked = locate(gripSpaces[h], hand.grip);
        if(!locate(aimSpaces[h], hand.aim))
        {
            hand.aim = hand.grip;
        }
    }
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
