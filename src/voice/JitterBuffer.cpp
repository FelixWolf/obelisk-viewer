#include "viewer/voice/JitterBuffer.hpp"

#include <opus.h>

#include <algorithm>
#include <cstring>

namespace viewer::voice
{

namespace
{

constexpr uint64_t kMs = JitterBuffer::kRate / 1000;
constexpr uint64_t kMinTarget = 60 * kMs;
constexpr uint64_t kMaxTarget = 240 * kMs;
constexpr uint64_t kExcess = 60 * kMs;   // over the target: too much
constexpr uint64_t kWindow = 2000 * kMs; // ... all through this long
constexpr int kMaxFrame = 5760;          // Opus's longest, 120 ms
constexpr int kMaxConcealed = 5;         // in a row, then it's dry
constexpr int64_t kMaxJump = 500;        // in sequence: it started anew
constexpr size_t kMaxPackets = 100;

} // namespace

std::optional<RtpPacket> parseRtp(std::span<const uint8_t> b)
{
    if(b.size() < 12 || (b[0] >> 6) != 2)
    {
        return std::nullopt;
    }
    if(b[1] >= 192 && b[1] <= 223)
    {
        return std::nullopt; // RTCP
    }
    RtpPacket p;
    p.payloadType = b[1] & 0x7F;
    p.sequence = static_cast<uint16_t>(b[2] << 8 | b[3]);
    p.timestamp = uint32_t(b[4]) << 24 | uint32_t(b[5]) << 16 | uint32_t(b[6]) << 8 | b[7];
    p.source = uint32_t(b[8]) << 24 | uint32_t(b[9]) << 16 | uint32_t(b[10]) << 8 | b[11];
    size_t at = 12 + size_t(b[0] & 0x0F) * 4;
    if(b[0] & 0x10)
    {
        if(b.size() < at + 4)
        {
            return std::nullopt;
        }
        at += 4 + (size_t(b[at + 2]) << 8 | b[at + 3]) * 4;
    }
    size_t end = b.size();
    if(b[0] & 0x20)
    {
        const size_t padding = b.back();
        if(padding == 0 || padding > end)
        {
            return std::nullopt;
        }
        end -= padding;
    }
    if(at > end)
    {
        return std::nullopt;
    }
    p.payload = b.subspan(at, end - at);
    return p;
}

void JitterBuffer::Destroy::operator()(OpusDecoder* decoder) const
{
    opus_decoder_destroy(decoder);
}

JitterBuffer::JitterBuffer() : target_(kMinTarget)
{
    int error = 0;
    decoder_.reset(opus_decoder_create(kRate, 2, &error));
    decoded_.reserve(size_t(kMaxFrame) * 2);
}

JitterBuffer::~JitterBuffer() = default;

void JitterBuffer::reset()
{
    packets_.clear();
    playing_ = false;
    concealedRun_ = 0;
    if(decoder_)
    {
        opus_decoder_ctl(decoder_.get(), OPUS_RESET_STATE);
    }
}

void JitterBuffer::push(std::span<const uint8_t> bytes)
{
    const auto p = parseRtp(bytes);
    if(!p || p->payload.empty())
    {
        return;
    }
    const int frames = opus_packet_get_nb_samples(
        p->payload.data(), static_cast<opus_int32>(p->payload.size()), kRate);
    if(frames <= 0)
    {
        return;
    }
    std::vector<uint8_t> payload(p->payload.begin(), p->payload.end());
    std::lock_guard lock(mutex_);
    stats_.received++;
    int64_t sequence = p->sequence;
    if(source_ == p->source)
    {
        // Unwrapped: the nearest to the newest.
        const auto delta = static_cast<int16_t>(
            static_cast<uint16_t>(p->sequence - static_cast<uint16_t>(newest_)));
        sequence = newest_ + delta;
        if(delta > kMaxJump || delta < -kMaxJump)
        {
            reset();
            newest_ = sequence;
        }
        newest_ = std::max(newest_, sequence);
    }
    else
    {
        reset();
        source_ = p->source;
        newest_ = sequence;
    }
    if(playing_ && sequence < next_)
    {
        stats_.late++;
        return;
    }
    if(!packets_.emplace(sequence, std::move(payload)).second)
    {
        stats_.late++;
        return;
    }
    frame_ = frames;
    while(packets_.size() > kMaxPackets)
    {
        packets_.erase(packets_.begin());
        stats_.skipped++;
    }
    if(playing_ && packets_.begin()->first > next_ && packets_.size() >= kMaxPackets)
    {
        next_ = packets_.begin()->first;
    }
}

uint64_t JitterBuffer::heldFrames() const
{
    return packets_.size() * static_cast<uint64_t>(frame_) + (decoded_.size() - decodedAt_) / 2;
}

bool JitterBuffer::decode(std::span<const uint8_t> packet, bool redundancy, int frames)
{
    decoded_.resize(size_t(frames) * 2);
    decodedAt_ = 0;
    const int n = opus_decode_float(decoder_.get(), packet.empty() ? nullptr : packet.data(),
                                    static_cast<opus_int32>(packet.size()), decoded_.data(), frames,
                                    redundancy ? 1 : 0);
    decoded_.resize(n > 0 ? size_t(n) * 2 : 0);
    return n > 0;
}

// The next packet's samples (or what stands in for them); none: dry.
void JitterBuffer::decodeNext()
{
    decoded_.clear();
    decodedAt_ = 0;
    if(const auto it = packets_.find(next_); it != packets_.end())
    {
        const std::vector<uint8_t> packet = std::move(it->second);
        packets_.erase(it);
        next_++;
        concealedRun_ = 0;
        if(!decode(packet, false, kMaxFrame))
        {
            decode({}, false, frame_);
        }
        return;
    }
    if(!packets_.empty() && packets_.begin()->first - next_ > kMaxConcealed)
    {
        next_ = packets_.begin()->first; // a gap too long to make up
        decodeNext();
        return;
    }
    if(concealedRun_ >= kMaxConcealed || frame_ <= 0)
    {
        return;
    }
    concealedRun_++;
    stats_.concealed++;
    const auto after = packets_.find(next_ + 1);
    if(after != packets_.end())
    {
        decode(after->second, true, frame_);
        next_++;
    }
    else
    {
        decode({}, false, frame_);
        if(!packets_.empty())
        {
            next_++; // lost (others after it came); else only late so far
        }
    }
}

uint64_t JitterBuffer::read(float* out, uint64_t frames)
{
    std::lock_guard lock(mutex_);
    uint64_t done = 0;
    while(decoder_ && done < frames)
    {
        if(!playing_)
        {
            if(packets_.empty() || heldFrames() < target_)
            {
                break;
            }
            playing_ = true;
            next_ = packets_.begin()->first;
            concealedRun_ = 0;
            lowest_ = heldFrames();
            window_ = 0;
        }
        if(decodedAt_ >= decoded_.size())
        {
            decodeNext();
            if(decoded_.empty())
            {
                // Dry: quiet until there's enough again, and more of it.
                playing_ = false;
                stats_.underruns++;
                target_ = std::min(target_ + 20 * kMs, kMaxTarget);
                break;
            }
        }
        const uint64_t n = std::min<uint64_t>(frames - done, (decoded_.size() - decodedAt_) / 2);
        std::memcpy(out + done * 2, decoded_.data() + decodedAt_, n * 2 * sizeof(float));
        decodedAt_ += n * 2;
        done += n;
    }
    std::memset(out + done * 2, 0, (frames - done) * 2 * sizeof(float));

    if(playing_)
    {
        lowest_ = std::min(lowest_, heldFrames());
        window_ += frames;
        if(window_ >= kWindow)
        {
            if(lowest_ > target_ + kExcess && packets_.size() > 2 && packets_.contains(next_))
            {
                packets_.erase(next_);
                next_++;
                stats_.skipped++;
            }
            else if(lowest_ > kMinTarget / 2 && target_ > kMinTarget)
            {
                target_ -= kMs; // steady: slowly back down
            }
            lowest_ = heldFrames();
            window_ = 0;
        }
    }
    return frames;
}

JitterBuffer::Stats JitterBuffer::stats() const
{
    std::lock_guard lock(mutex_);
    Stats s = stats_;
    s.heldMs = static_cast<float>(heldFrames()) / kMs;
    s.targetMs = static_cast<float>(target_) / kMs;
    return s;
}

} // namespace viewer::voice
