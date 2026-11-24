#include "viewer/audio/AudioStream.hpp"

#include "viewer/core/Log.hpp"

#include <miniaudio.h>
#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>

namespace viewer::audio
{

namespace
{

bool startsWith(std::string_view s, std::string_view prefix)
{
    if(s.size() < prefix.size())
    {
        return false;
    }
    for(size_t i = 0; i < prefix.size(); i++)
    {
        const auto lower = [](char c)
        { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        if(lower(s[i]) != prefix[i])
        {
            return false;
        }
    }
    return true;
}

} // namespace

struct AudioStream::Ring
{
    ma_pcm_rb rb{};
    ~Ring() { ma_pcm_rb_uninit(&rb); }
};

// The decoder thread's: a resampler when the source's rate isn't the
// output's (a chained stream's later part), and scratch space.
struct AudioStream::Decoding
{
    ma_linear_resampler resampler{};
    bool resampling = false;
    uint32_t from = 0;
    std::vector<float> stereo;
    std::vector<float> resampled;

    ~Decoding()
    {
        if(resampling)
        {
            ma_linear_resampler_uninit(&resampler, nullptr);
        }
    }
};

AudioStream::Format AudioStream::sniff(std::string_view type, std::span<const uint8_t> b)
{
    if(startsWith(type, "audio/mpeg") || startsWith(type, "audio/mp3") ||
       startsWith(type, "audio/x-mpeg"))
    {
        return Format::Mp3;
    }
    if(startsWith(type, "audio/ogg") || startsWith(type, "application/ogg") ||
       startsWith(type, "audio/vorbis") || startsWith(type, "audio/x-ogg"))
    {
        return Format::Vorbis;
    }
    if(startsWith(type, "audio/aac") || startsWith(type, "audio/x-aac") ||
       startsWith(type, "audio/mp4") || startsWith(type, "audio/aacp"))
    {
        return Format::Aac;
    }
    // Not said (or said wrong, as "application/octet-stream"): by the bytes.
    if(b.size() < 4)
    {
        return Format::Unknown;
    }
    if(b[0] == 'O' && b[1] == 'g' && b[2] == 'g' && b[3] == 'S')
    {
        return Format::Vorbis;
    }
    if(b[0] == 'I' && b[1] == 'D' && b[2] == '3')
    {
        return Format::Mp3; // a tag first: MP3's
    }
    // Frame sync (11 bits): MPEG audio's layer is never 0, ADTS (AAC) always.
    for(size_t i = 0; i + 1 < b.size(); i++)
    {
        if(b[i] == 0xFF && (b[i + 1] & 0xE0) == 0xE0)
        {
            return (b[i + 1] & 0x06) == 0 ? Format::Aac : Format::Mp3;
        }
    }
    return b.size() >= 4096 ? Format::Other : Format::Unknown;
}

std::string_view AudioStream::formatName(Format format)
{
    switch(format)
    {
    case Format::Mp3:
        return "MP3";
    case Format::Vorbis:
        return "Ogg Vorbis";
    case Format::Aac:
        return "AAC";
    case Format::Other:
        return "unknown";
    case Format::Unknown:
        break;
    }
    return "";
}

AudioStream::AudioStream(std::string contentType)
    : contentType_(std::move(contentType)), decoding_(std::make_unique<Decoding>())
{
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

AudioStream::~AudioStream()
{
    thread_.request_stop();
    arrived_.notify_all();
    thread_.join();
}

void AudioStream::push(std::span<const uint8_t> bytes)
{
    {
        std::lock_guard lock(mutex_);
        if(pending_.size() - pendingAt_ + bytes.size() > kMaxPendingBytes)
        {
            // Not keeping up (or not playing): the oldest go.
            pending_.clear();
            pendingAt_ = 0;
        }
        else if(pendingAt_ > 0 && pendingAt_ * 2 > pending_.size())
        {
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<ptrdiff_t>(pendingAt_));
            pendingAt_ = 0;
        }
        pending_.insert(pending_.end(), bytes.begin(), bytes.end());
    }
    arrived_.notify_all();
}

void AudioStream::finish()
{
    {
        std::lock_guard lock(mutex_);
        finished_ = true;
    }
    arrived_.notify_all();
}

AudioStream::State AudioStream::state() const
{
    if(failed_.load(std::memory_order_acquire))
    {
        return State::Failed;
    }
    if(drained_.load(std::memory_order_acquire))
    {
        return State::Ended;
    }
    if(!ready_.load(std::memory_order_acquire) || buffering_.load(std::memory_order_relaxed))
    {
        return State::Buffering;
    }
    return State::Playing;
}

std::string AudioStream::error() const
{
    std::lock_guard lock(mutex_);
    return error_;
}

float AudioStream::bufferedSeconds() const
{
    if(!ready_.load(std::memory_order_acquire) || rate_ == 0)
    {
        return 0.0f;
    }
    const ma_uint32 frames = ma_pcm_rb_available_read(&ring_->rb);
    return static_cast<float>(frames) / static_cast<float>(rate_);
}

void AudioStream::fail(std::string why)
{
    {
        std::lock_guard lock(mutex_);
        if(error_.empty())
        {
            error_ = std::move(why);
        }
    }
    failed_.store(true, std::memory_order_release);
}

size_t AudioStream::pull(uint8_t* out, size_t n)
{
    std::unique_lock lock(mutex_);
    const std::stop_token stop = thread_.get_stop_token();
    arrived_.wait(lock, stop, [&] { return pendingAt_ < pending_.size() || finished_; });
    if(stop.stop_requested())
    {
        return 0;
    }
    const size_t take = std::min(n, pending_.size() - pendingAt_);
    std::memcpy(out, pending_.data() + pendingAt_, take);
    pendingAt_ += take;
    return take;
}

uint64_t AudioStream::read(float* out, uint64_t frames)
{
    if(!ready_.load(std::memory_order_acquire))
    {
        std::memset(out, 0, frames * 2 * sizeof(float));
        return frames;
    }
    ma_pcm_rb& rb = ring_->rb;
    const bool done = decoded_.load(std::memory_order_acquire);
    if(buffering_.load(std::memory_order_relaxed))
    {
        if(ma_pcm_rb_available_read(&rb) < prebufferFrames_ && !done)
        {
            std::memset(out, 0, frames * 2 * sizeof(float));
            return frames;
        }
        buffering_.store(false, std::memory_order_relaxed);
    }
    uint64_t got = 0;
    while(got < frames)
    {
        ma_uint32 n = static_cast<ma_uint32>(std::min<uint64_t>(frames - got, 1u << 16));
        void* from = nullptr;
        if(ma_pcm_rb_acquire_read(&rb, &n, &from) != MA_SUCCESS || n == 0)
        {
            break;
        }
        std::memcpy(out + got * 2, from, n * 2 * sizeof(float));
        ma_pcm_rb_commit_read(&rb, n);
        got += n;
    }
    if(got < frames)
    {
        std::memset(out + got * 2, 0, (frames - got) * 2 * sizeof(float));
        if(done)
        {
            if(got == 0)
            {
                drained_.store(true, std::memory_order_release);
            }
            return got;
        }
        // Ran dry: quiet until there's enough again.
        buffering_.store(true, std::memory_order_relaxed);
        underruns_.fetch_add(1, std::memory_order_relaxed);
        return frames;
    }
    return frames;
}

bool AudioStream::writeStereo(const float* stereo, uint64_t frames)
{
    ma_pcm_rb& rb = ring_->rb;
    const std::stop_token stop = thread_.get_stop_token();
    uint64_t put = 0;
    while(put < frames)
    {
        if(stop.stop_requested())
        {
            return false;
        }
        ma_uint32 n = static_cast<ma_uint32>(std::min<uint64_t>(frames - put, 1u << 16));
        void* to = nullptr;
        if(ma_pcm_rb_acquire_write(&rb, &n, &to) != MA_SUCCESS || n == 0)
        {
            // Full: it's ahead of what plays.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        std::memcpy(to, stereo + put * 2, n * 2 * sizeof(float));
        ma_pcm_rb_commit_write(&rb, n);
        put += n;
    }
    return true;
}

bool AudioStream::write(const float* samples, uint64_t frames, uint32_t channels, uint32_t rate)
{
    if(frames == 0 || channels == 0 || rate == 0)
    {
        return true;
    }
    Decoding& d = *decoding_;
    if(!ready_.load(std::memory_order_relaxed))
    {
        // The first samples set the output's rate.
        rate_ = rate;
        prebufferFrames_ = static_cast<uint32_t>(kPrebufferSeconds * static_cast<float>(rate));
        auto ring = std::make_unique<Ring>();
        const auto size = static_cast<ma_uint32>(kBufferSeconds * static_cast<float>(rate));
        if(ma_pcm_rb_init(ma_format_f32, 2, size, nullptr, nullptr, &ring->rb) != MA_SUCCESS)
        {
            fail("out of memory");
            return false;
        }
        ring_ = std::move(ring);
        ready_.store(true, std::memory_order_release);
    }
    channels_.store(channels, std::memory_order_relaxed);

    // To stereo: mono doubled, more than two: the first two.
    d.stereo.resize(frames * 2);
    for(uint64_t f = 0; f < frames; f++)
    {
        const float l = samples[f * channels];
        const float r = channels > 1 ? samples[f * channels + 1] : l;
        d.stereo[f * 2] = l;
        d.stereo[f * 2 + 1] = r;
    }
    if(rate == rate_)
    {
        return writeStereo(d.stereo.data(), frames);
    }
    // At another rate (a chained stream's next part): to the output's.
    if(!d.resampling || d.from != rate)
    {
        if(d.resampling)
        {
            ma_linear_resampler_uninit(&d.resampler, nullptr);
            d.resampling = false;
        }
        const ma_linear_resampler_config config =
            ma_linear_resampler_config_init(ma_format_f32, 2, rate, rate_);
        if(ma_linear_resampler_init(&config, nullptr, &d.resampler) != MA_SUCCESS)
        {
            return true; // skipped
        }
        d.resampling = true;
        d.from = rate;
    }
    ma_uint64 in = frames;
    ma_uint64 outFrames = frames * rate_ / rate + 16;
    d.resampled.resize(outFrames * 2);
    ma_linear_resampler_process_pcm_frames(&d.resampler, d.stereo.data(), &in, d.resampled.data(),
                                           &outFrames);
    return writeStereo(d.resampled.data(), outFrames);
}

namespace
{

ma_result mp3Read(ma_decoder* decoder, void* out, size_t bytes, size_t* read)
{
    auto* pull = static_cast<std::function<size_t(uint8_t*, size_t)>*>(decoder->pUserData);
    size_t got = 0;
    // As much as asked for (the decoder takes a short read as the end).
    while(got < bytes)
    {
        const size_t n = (*pull)(static_cast<uint8_t*>(out) + got, bytes - got);
        if(n == 0)
        {
            break;
        }
        got += n;
    }
    *read = got;
    return got == 0 ? MA_AT_END : MA_SUCCESS;
}

ma_result mp3Seek(ma_decoder*, ma_int64, ma_seek_origin)
{
    return MA_NOT_IMPLEMENTED; // a stream: no going back
}

size_t vorbisRead(void* out, size_t size, size_t count, void* source)
{
    auto* pull = static_cast<std::function<size_t(uint8_t*, size_t)>*>(source);
    return size == 0 ? 0 : (*pull)(static_cast<uint8_t*>(out), size * count) / size;
}

} // namespace

void AudioStream::decodeMp3()
{
    std::function<size_t(uint8_t*, size_t)> pull = [this](uint8_t* out, size_t n)
    { return this->pull(out, n); };
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    config.encodingFormat = ma_encoding_format_mp3;
    ma_decoder decoder;
    if(ma_decoder_init(mp3Read, mp3Seek, &pull, &config, &decoder) != MA_SUCCESS)
    {
        if(!thread_.get_stop_token().stop_requested())
        {
            fail("not an MP3 stream");
        }
        return;
    }
    ma_format format;
    ma_uint32 channels = 0;
    ma_uint32 rate = 0;
    ma_decoder_get_data_format(&decoder, &format, &channels, &rate, nullptr, 0);
    std::vector<float> samples;
    constexpr ma_uint64 kChunk = 1152 * 4;
    for(;;)
    {
        samples.resize(kChunk * std::max<ma_uint32>(channels, 1));
        ma_uint64 frames = 0;
        const ma_result result =
            ma_decoder_read_pcm_frames(&decoder, samples.data(), kChunk, &frames);
        if(frames > 0 && !write(samples.data(), frames, channels, rate))
        {
            break;
        }
        if(result != MA_SUCCESS || frames == 0)
        {
            break;
        }
    }
    ma_decoder_uninit(&decoder);
}

void AudioStream::decodeVorbis()
{
    std::function<size_t(uint8_t*, size_t)> pull = [this](uint8_t* out, size_t n)
    { return this->pull(out, n); };
    // No seeking: libvorbisfile reads it as a stream, chained parts and all.
    const ov_callbacks callbacks{vorbisRead, nullptr, nullptr, nullptr};
    OggVorbis_File file;
    if(ov_open_callbacks(&pull, &file, nullptr, 0, callbacks) != 0)
    {
        if(!thread_.get_stop_token().stop_requested())
        {
            fail("not an Ogg Vorbis stream");
        }
        return;
    }
    std::vector<float> samples;
    int section = 0;
    for(;;)
    {
        float** pcm = nullptr;
        const long frames = ov_read_float(&file, &pcm, 4096, &section);
        if(frames == OV_HOLE)
        {
            continue; // a gap (dropped bytes): carries on
        }
        if(frames <= 0)
        {
            break;
        }
        const vorbis_info* info = ov_info(&file, -1);
        if(!info || info->channels < 1)
        {
            break;
        }
        const auto channels = static_cast<uint32_t>(info->channels);
        samples.resize(static_cast<size_t>(frames) * channels);
        for(long f = 0; f < frames; f++)
        {
            for(uint32_t c = 0; c < channels; c++)
            {
                samples[static_cast<size_t>(f) * channels + c] = std::clamp(pcm[c][f], -1.0f, 1.0f);
            }
        }
        if(!write(samples.data(), static_cast<uint64_t>(frames), channels,
                  static_cast<uint32_t>(info->rate)))
        {
            break;
        }
    }
    ov_clear(&file);
}

void AudioStream::run(std::stop_token stop)
{
    // Which it is: the Content-Type, or else the first bytes.
    Format format = sniff(contentType_, {});
    while(format == Format::Unknown && !stop.stop_requested())
    {
        std::unique_lock lock(mutex_);
        arrived_.wait(lock, stop,
                      [&] { return pending_.size() - pendingAt_ >= 4096 || finished_; });
        format = sniff({}, std::span(pending_).subspan(pendingAt_));
        if(format == Format::Unknown && finished_)
        {
            format = Format::Other;
        }
    }
    format_.store(format, std::memory_order_release);
    switch(format)
    {
    case Format::Mp3:
        decodeMp3();
        break;
    case Format::Vorbis:
        decodeVorbis();
        break;
    case Format::Aac:
        fail("AAC streams can't be played yet");
        break;
    case Format::Other:
        fail(contentType_.empty() ? "not an audio stream"
                                  : "not an audio stream (" + contentType_ + ")");
        break;
    case Format::Unknown:
        break; // stopping
    }
    if(!stop.stop_requested() && !failed_.load() && !ready_.load())
    {
        fail("the stream ended before any sound");
    }
    decoded_.store(true, std::memory_order_release);
}

} // namespace viewer::audio
