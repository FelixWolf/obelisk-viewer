#pragma once

// A connection's sound as it plays: RTP packets go in as they arrive, in
// whatever order (the network's thread), and come out decoded (Opus) at an
// even pace (the audio thread). Holds a little before starting; lost packets
// are made up (from the next one's redundancy, else guessed), running dry
// makes it hold more, and too much held for a while is played off by
// leaving a packet out.

#include "viewer/audio/PcmSource.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

struct OpusDecoder;

namespace viewer::voice
{

struct RtpPacket
{
    uint8_t payloadType = 0;
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    uint32_t source = 0;
    std::span<const uint8_t> payload;
};
// Nothing if it isn't one (RTCP, or cut short).
std::optional<RtpPacket> parseRtp(std::span<const uint8_t> bytes);

class JitterBuffer final : public audio::PcmSource
{
public:
    static constexpr uint32_t kRate = 48000;

    JitterBuffer();
    ~JitterBuffer() override;

    // A packet as it arrived (RTP); anything else is ignored.
    void push(std::span<const uint8_t> bytes);

    uint32_t sampleRate() const override { return kRate; }
    uint64_t read(float* out, uint64_t frames) override;

    struct Stats
    {
        uint64_t received = 0;
        uint64_t concealed = 0; // packets made up
        uint64_t late = 0;      // too late to play, or twice
        uint64_t skipped = 0;   // left out to catch up
        uint64_t underruns = 0;
        float heldMs = 0.0f;
        float targetMs = 0.0f;
    };
    Stats stats() const;

private:
    void reset(); // (locked)
    uint64_t heldFrames() const;
    void decodeNext();
    bool decode(std::span<const uint8_t> packet, bool redundancy, int frames);

    mutable std::mutex mutex_;
    struct Destroy
    {
        void operator()(OpusDecoder* decoder) const;
    };
    std::unique_ptr<OpusDecoder, Destroy> decoder_;
    std::map<int64_t, std::vector<uint8_t>> packets_; // by sequence, unwrapped
    std::optional<uint32_t> source_;
    uint8_t payloadType_ = 0;
    int64_t newest_ = 0; // the highest sequence yet
    int64_t next_ = 0;   // the one to play
    bool playing_ = false;
    int frame_ = 960; // the last packet's frames
    uint64_t target_;
    int concealedRun_ = 0;
    uint64_t lowest_ = 0; // the least held, this window
    uint64_t window_ = 0; // frames read in it
    std::vector<float> decoded_;
    size_t decodedAt_ = 0; // samples of it read
    Stats stats_;
};

} // namespace viewer::voice
