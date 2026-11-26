#include "viewer/media/MediaHost.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/core/MessageQueue.hpp"
#include "viewer/media/ChildProcess.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace viewer::media
{

TabAudio::TabAudio(uint32_t sampleRate, uint32_t channels)
    : rate_(sampleRate), channels_(std::max(channels, 1u)),
      ring_(2 * static_cast<size_t>(kMaxSeconds * static_cast<float>(sampleRate)))
{
}

void TabAudio::push(std::span<const float> samples)
{
    std::lock_guard lock(mutex_);
    const size_t capacity = ring_.size() / 2;
    if(capacity == 0)
    {
        return;
    }
    for(size_t i = 0; i + channels_ <= samples.size(); i += channels_)
    {
        const float left = samples[i];
        const float right = channels_ > 1 ? samples[i + 1] : left;
        ring_[2 * head_] = left;
        ring_[2 * head_ + 1] = right;
        head_ = (head_ + 1) % capacity;
        size_ = std::min(size_ + 1, capacity); // full: the oldest goes
    }
}

void TabAudio::end()
{
    std::lock_guard lock(mutex_);
    ended_ = true;
}

float TabAudio::bufferedSeconds() const
{
    std::lock_guard lock(mutex_);
    return rate_ ? static_cast<float>(size_) / static_cast<float>(rate_) : 0.0f;
}

uint64_t TabAudio::read(float* out, uint64_t frames)
{
    std::lock_guard lock(mutex_);
    if(ended_)
    {
        return 0;
    }
    const size_t capacity = ring_.size() / 2;
    if(!playing_ && static_cast<float>(size_) >= kPrebufferSeconds * static_cast<float>(rate_))
    {
        playing_ = true;
    }
    uint64_t i = 0;
    if(playing_ && capacity > 0)
    {
        size_t tail = (head_ + capacity - size_) % capacity;
        for(; i < frames && size_ > 0; i++, size_--)
        {
            out[2 * i] = ring_[2 * tail];
            out[2 * i + 1] = ring_[2 * tail + 1];
            tail = (tail + 1) % capacity;
        }
        if(size_ == 0)
        {
            playing_ = false; // dry: wait for enough again
        }
    }
    std::fill(out + 2 * i, out + 2 * frames, 0.0f);
    return frames;
}

namespace
{

class ProcessHost final : public MediaHost
{
public:
    ProcessHost(std::unique_ptr<ChildProcess> process, const Init& init)
        : process_(std::move(process))
    {
        if(!process_->write(encode(init)))
        {
            fail("it couldn't be told how to start");
            return;
        }
        reader_ = std::thread([this] { read(); });
    }
    ~ProcessHost() override
    {
        process_->write(encode(Quit{}));
        // Its output ends once it's gone (killed if it takes too long),
        // which ends the reader; only then is it let go.
        process_->finish();
        if(reader_.joinable())
        {
            reader_.join();
        }
        process_.reset();
    }

    State state() const override { return state_.load(); }
    std::string error() const override
    {
        std::lock_guard lock(mutex_);
        return error_;
    }
    bool send(const Message& message) override
    {
        return state_.load() != State::Failed && process_->write(encode(message));
    }
    void take(std::vector<Message>& out) override { said_.drain(out); }
    std::shared_ptr<TabAudio> audio(TabId tab) override
    {
        std::lock_guard lock(mutex_);
        const auto it = audio_.find(tab);
        return it == audio_.end() ? nullptr : it->second;
    }

private:
    void fail(std::string why)
    {
        {
            std::lock_guard lock(mutex_);
            if(error_.empty())
            {
                error_ = std::move(why);
            }
        }
        state_ = State::Failed;
    }

    // The reader thread.
    void read()
    {
        Parser parser;
        std::vector<uint8_t> buffer(256 * 1024);
        for(;;)
        {
            const size_t n = process_->read(buffer);
            if(n == 0)
            {
                fail(state_.load() == State::Starting ? "it exited while starting" : "it exited");
                break;
            }
            parser.feed(std::span(buffer).first(n));
            while(auto m = parser.next())
            {
                handle(std::move(*m));
            }
            if(parser.broken())
            {
                fail("it said something that isn't a message");
                break;
            }
        }
        // Whatever sound there was stops.
        std::lock_guard lock(mutex_);
        for(auto& [tab, a] : audio_)
        {
            a->end();
        }
        audio_.clear();
    }

    void handle(Message&& m)
    {
        if(const auto* ready = std::get_if<Ready>(&m))
        {
            if(ready->version != kProtocolVersion)
            {
                fail(std::format("it speaks version {}, not {}", ready->version, kProtocolVersion));
                return;
            }
            state_ = State::Ready;
        }
        else if(const auto* failed = std::get_if<Failed>(&m))
        {
            fail(failed->reason);
        }
        else if(const auto* start = std::get_if<AudioStart>(&m))
        {
            std::lock_guard lock(mutex_);
            auto& a = audio_[start->tab];
            if(a)
            {
                a->end();
            }
            a = std::make_shared<TabAudio>(start->sampleRate, start->channels);
        }
        else if(const auto* samples = std::get_if<Audio>(&m))
        {
            std::shared_ptr<TabAudio> a = audio(samples->tab);
            if(a)
            {
                a->push(samples->samples);
            }
            return; // not for the main thread
        }
        else if(const auto* closed = std::get_if<Closed>(&m))
        {
            std::lock_guard lock(mutex_);
            if(const auto it = audio_.find(closed->tab); it != audio_.end())
            {
                it->second->end();
                audio_.erase(it);
            }
        }
        said_.push(std::move(m));
    }

    std::unique_ptr<ChildProcess> process_;
    std::atomic<State> state_{State::Starting};
    mutable std::mutex mutex_;
    std::string error_;
    std::unordered_map<TabId, std::shared_ptr<TabAudio>> audio_;
    MessageQueue<Message> said_;
    std::thread reader_; // last
};

} // namespace

std::unique_ptr<MediaHost> startMediaHost(const std::filesystem::path& program, Init init)
{
    auto process = ChildProcess::start(program, {});
    if(!process)
    {
        log::warn("media: {} couldn't be started", program.string());
        return nullptr;
    }
    return std::make_unique<ProcessHost>(std::move(process), init);
}

} // namespace viewer::media
