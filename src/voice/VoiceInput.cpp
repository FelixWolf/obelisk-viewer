#include "viewer/voice/VoiceInput.hpp"

#include "viewer/audio/Microphone.hpp"
#include "viewer/core/Log.hpp"

#include <opus.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <optional>

namespace viewer::voice
{

namespace
{

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr int kRate = 48000;
constexpr size_t kFrame = 960; // 20 ms: a packet
constexpr size_t kBlock = 480; // 10 ms: what SpeexDSP takes
constexpr int kEchoTail = kRate / 5;
// What's played waits here for the microphone's sound it's in; more than
// this and it's dropped down to the least.
constexpr size_t kTapMost = kRate / 10;
constexpr size_t kTapLeast = kRate / 50;

using Block = std::array<spx_int16_t, kBlock>;

// Echo, noise and level.
class Processor
{
public:
    Processor()
        : echo_(speex_echo_state_init(kBlock, kEchoTail)),
          preprocess_(speex_preprocess_state_init(kBlock, kRate))
    {
        int rate = kRate;
        speex_echo_ctl(echo_, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    }
    ~Processor()
    {
        speex_preprocess_state_destroy(preprocess_);
        speex_echo_state_destroy(echo_);
    }
    Processor(const Processor&) = delete;
    Processor& operator=(const Processor&) = delete;

    void set(const VoiceInput::Processing& p)
    {
        useEcho_ = p.echo;
        void* echo = p.echo ? echo_ : nullptr;
        speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_ECHO_STATE, echo);
        int on = p.noise;
        speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_DENOISE, &on);
        int suppress = -25; // dB
        speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &suppress);
        on = p.level;
        speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_AGC, &on);
        int gain = 18; // dB at most
        speex_preprocess_ctl(preprocess_, SPEEX_PREPROCESS_SET_AGC_MAX_GAIN, &gain);
        preprocess = p.noise || p.level || p.echo;
    }

    void run(Block& sound, const Block& played)
    {
        if(useEcho_)
        {
            Block out;
            speex_echo_cancellation(echo_, sound.data(), played.data(), out.data());
            sound = out;
        }
        if(preprocess)
        {
            speex_preprocess_run(preprocess_, sound.data());
        }
    }

private:
    SpeexEchoState* echo_;
    SpeexPreprocessState* preprocess_;
    bool useEcho_ = true;
    bool preprocess = true;
};

spx_int16_t sample(float v)
{
    return static_cast<spx_int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}

struct Encoder
{
    OpusEncoder* state = nullptr;

    Encoder()
    {
        int error = 0;
        state = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &error);
        if(state)
        {
            opus_encoder_ctl(state, OPUS_SET_BITRATE(40000));
            opus_encoder_ctl(state, OPUS_SET_INBAND_FEC(1));
            opus_encoder_ctl(state, OPUS_SET_PACKET_LOSS_PERC(5));
        }
    }
    ~Encoder()
    {
        if(state)
        {
            opus_encoder_destroy(state);
        }
    }
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    std::vector<uint8_t> encode(const std::array<float, kFrame>& frame)
    {
        std::vector<uint8_t> out(1275);
        const opus_int32 n = state ? opus_encode_float(state, frame.data(), kFrame, out.data(),
                                                       static_cast<opus_int32>(out.size()))
                                   : 0;
        out.resize(n > 0 ? static_cast<size_t>(n) : 0);
        return out;
    }
};

} // namespace

VoiceInput::VoiceInput(audio::AudioEngine* engine)
    : engine_(engine), thread_([this](const std::stop_token& stop) { run(stop); })
{
}

VoiceInput::~VoiceInput()
{
    thread_.request_stop();
    thread_.join();
}

void VoiceInput::setSinks(std::vector<Sink> sinks)
{
    std::lock_guard lock(mutex_);
    sinks_ = std::move(sinks);
    changed_.notify_all();
}

void VoiceInput::setTalking(bool talking)
{
    talking_.store(talking, std::memory_order_relaxed);
    if(talking && !wanted_.exchange(true))
    {
        failed_.store(false, std::memory_order_relaxed);
        std::lock_guard lock(mutex_);
        changed_.notify_all();
    }
}

void VoiceInput::release()
{
    talking_.store(false, std::memory_order_relaxed);
    wanted_.store(false);
}

void VoiceInput::setDevice(const std::string& name)
{
    std::lock_guard lock(mutex_);
    if(name != device_)
    {
        device_ = name;
        deviceVersion_++;
    }
}

void VoiceInput::setProcessing(const Processing& processing)
{
    std::lock_guard lock(mutex_);
    processing_ = processing;
}

void VoiceInput::run(const std::stop_token& stop)
{
    Encoder encoder;
    const std::vector<uint8_t> silence = Encoder().encode({});
    Processor processor;
    std::unique_ptr<audio::Microphone> mic;
    uint64_t micVersion = 0;
    Clock::time_point reopenAt{};
    Clock::time_point lastSound{}; // the microphone's
    Clock::time_point due = Clock::now();
    uint32_t timestamp = 0;
    float gain = 0.0f; // eased, so muting doesn't click

    while(!stop.stop_requested())
    {
        std::vector<Sink> sinks;
        std::string device;
        uint64_t version = 0;
        {
            std::unique_lock lock(mutex_);
            if(sinks_.empty() && !wanted_.load())
            {
                mic.reset();
                open_.store(false, std::memory_order_relaxed);
                changed_.wait(lock, stop, [this] { return !sinks_.empty() || wanted_.load(); });
                due = Clock::now();
                continue;
            }
            sinks = sinks_;
            device = device_;
            version = deviceVersion_;
            processor.set(processing_);
        }
        Clock::time_point now = Clock::now();

        // Open, close or change the microphone.
        if(mic && (!wanted_.load() || version != micVersion))
        {
            mic.reset();
            reopenAt = {};
        }
        if(!mic && wanted_.load() && now >= reopenAt)
        {
            mic = audio::Microphone::open(device);
            micVersion = version;
            reopenAt = Clock::now() + 5s;
            lastSound = Clock::now();
            failed_.store(!mic, std::memory_order_relaxed);
            if(engine_)
            {
                engine_->tap().keep(0);
            }
        }
        open_.store(mic != nullptr, std::memory_order_relaxed);
        if(engine_)
        {
            engine_->setTapped(mic != nullptr);
        }

        std::array<float, kFrame> frame{};
        bool sound = false;
        now = Clock::now();
        if(mic && mic->samples().read(frame))
        {
            sound = true;
            lastSound = now;
            due = now + 60ms; // the microphone paces; this if it stalls
            mic->samples().keep(kRate / 5);
        }
        else if(mic && now - lastSound > 3s)
        {
            log::warn("microphone: nothing comes from it; opening it again");
            mic.reset();
            continue;
        }
        else if(now < due)
        {
            std::this_thread::sleep_for(std::min<Clock::duration>(due - now, 4ms));
            continue;
        }
        else
        {
            // Silence keeps its own time.
            due += 20ms;
            if(due < now - 200ms)
            {
                due = now + 20ms;
            }
        }

        float power = 0.0f;
        if(sound)
        {
            for(size_t b = 0; b < kFrame / kBlock; b++)
            {
                Block in;
                Block played{};
                std::array<float, kBlock> tap{};
                if(engine_)
                {
                    if(engine_->tap().available() > kTapMost)
                    {
                        engine_->tap().keep(kTapLeast);
                    }
                    engine_->tap().read(tap);
                }
                for(size_t i = 0; i < kBlock; i++)
                {
                    in[i] = sample(frame[b * kBlock + i]);
                    played[i] = sample(tap[i]);
                }
                processor.run(in, played);
                for(size_t i = 0; i < kBlock; i++)
                {
                    frame[b * kBlock + i] = static_cast<float>(in[i]) / 32768.0f;
                }
            }
            const float target = talking_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
            const float from = gain;
            for(size_t i = 0; i < kFrame; i++)
            {
                const float t = static_cast<float>(i + 1) / static_cast<float>(kFrame);
                frame[i] *= from + (target - from) * t;
                power += frame[i] * frame[i];
            }
            gain = target;
            power = std::sqrt(power / static_cast<float>(kFrame));
        }
        else
        {
            gain = 0.0f;
        }
        power_.store(power, std::memory_order_relaxed);

        const std::vector<uint8_t> packet = encoder.encode(frame);
        for(const Sink& s : sinks)
        {
            const std::vector<uint8_t>& p = s.live ? packet : silence;
            if(!p.empty())
            {
                s.peer->sendAudio(p, timestamp);
            }
        }
        timestamp += static_cast<uint32_t>(kFrame);
    }
    if(engine_)
    {
        engine_->setTapped(false);
    }
}

} // namespace viewer::voice
