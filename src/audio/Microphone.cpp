#include "viewer/audio/Microphone.hpp"

#include "viewer/core/Log.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace viewer::audio
{

namespace
{

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// PulseAudio and PipeWire list what's played, to be recorded, as inputs too.
bool isMonitor(std::string_view name)
{
    return name.starts_with("Monitor of ");
}

} // namespace

struct Microphone::Impl
{
    ma_context context{};
    ma_device device{};
    bool contextReady = false;
    bool deviceReady = false;

    ~Impl()
    {
        if(deviceReady)
        {
            ma_device_uninit(&device);
        }
        if(contextReady)
        {
            ma_context_uninit(&context);
        }
    }
};

Microphone::Microphone()
    : ring_(std::make_unique<FrameRing>(kSampleRate / 2)), impl_(std::make_unique<Impl>())
{
}

Microphone::~Microphone() = default;

std::vector<std::string> Microphone::names()
{
    Impl impl;
    if(ma_context_init(nullptr, 0, nullptr, &impl.context) != MA_SUCCESS)
    {
        return {};
    }
    impl.contextReady = true;
    ma_device_info* devices = nullptr;
    ma_uint32 count = 0;
    std::vector<std::string> out;
    if(ma_context_get_devices(&impl.context, nullptr, nullptr, &devices, &count) == MA_SUCCESS)
    {
        for(ma_uint32 i = 0; i < count; i++)
        {
            if(!isMonitor(devices[i].name))
            {
                out.emplace_back(devices[i].name);
            }
        }
    }
    return out;
}

std::optional<size_t> Microphone::match(const std::vector<std::string>& names,
                                        const std::string& name)
{
    if(name.empty())
    {
        return std::nullopt;
    }
    if(const auto it = std::ranges::find(names, name); it != names.end())
    {
        return static_cast<size_t>(it - names.begin());
    }
    const auto it = std::ranges::find_if(names, [wanted = lower(name)](const std::string& n)
                                         { return lower(n).find(wanted) != std::string::npos; });
    return it == names.end() ? std::nullopt
                             : std::optional(static_cast<size_t>(it - names.begin()));
}

std::unique_ptr<Microphone> Microphone::open(const std::string& name)
{
    std::unique_ptr<Microphone> mic(new Microphone);
    Impl& impl = *mic->impl_;
    if(ma_context_init(nullptr, 0, nullptr, &impl.context) != MA_SUCCESS)
    {
        log::warn("microphone: no audio input");
        return nullptr;
    }
    impl.contextReady = true;

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    ma_device_id id{};
    if(!name.empty())
    {
        ma_device_info* devices = nullptr;
        ma_uint32 count = 0;
        if(ma_context_get_devices(&impl.context, nullptr, nullptr, &devices, &count) == MA_SUCCESS)
        {
            std::vector<std::string> found;
            for(ma_uint32 i = 0; i < count; i++)
            {
                found.emplace_back(devices[i].name);
            }
            if(const auto which = match(found, name))
            {
                id = devices[*which].id;
                config.capture.pDeviceID = &id;
            }
        }
        if(!config.capture.pDeviceID)
        {
            log::warn("microphone: none named '{}'; using the default", name);
        }
    }
    config.capture.format = ma_format_f32;
    config.capture.channels = 1;
    config.sampleRate = kSampleRate;
    config.periodSizeInMilliseconds = 10;
    config.pUserData = mic->ring_.get();
    config.dataCallback = [](ma_device* device, void*, const void* in, ma_uint32 frames)
    {
        static_cast<FrameRing*>(device->pUserData)
            ->write(std::span(static_cast<const float*>(in), frames));
    };
    if(ma_device_init(&impl.context, &config, &impl.device) != MA_SUCCESS)
    {
        log::warn("microphone: couldn't open it");
        return nullptr;
    }
    impl.deviceReady = true;
    if(ma_device_start(&impl.device) != MA_SUCCESS)
    {
        log::warn("microphone: couldn't start it");
        return nullptr;
    }
    mic->name_ = impl.device.capture.name;
    log::info("microphone: '{}' open", mic->name_);
    return mic;
}

} // namespace viewer::audio
