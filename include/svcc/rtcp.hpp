// RTCP sender and receiver reports (RFC 3550 section 6.4), NTP time helpers, round-trip time
// from LSR/DLSR, and audio/video lip synchronisation through the SR NTP-to-RTP mapping.
#pragma once

#include "svcc/sim.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <variant>
#include <vector>

namespace svcc {

struct RtcpError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ReportBlock {
    std::uint32_t ssrc = 0;
    std::uint8_t fractionLost = 0;
    std::int32_t cumulativeLost = 0;  // 24-bit signed on the wire
    std::uint32_t extendedHighestSeq = 0;
    std::uint32_t jitter = 0;
    std::uint32_t lastSr = 0;       // middle 32 bits of the NTP time of the last SR received
    std::uint32_t delaySinceLastSr = 0;  // in 1/65536 s
};

struct SenderReport {
    std::uint32_t ssrc = 0;
    std::uint64_t ntp = 0;
    std::uint32_t rtpTimestamp = 0;
    std::uint32_t packetCount = 0;
    std::uint32_t octetCount = 0;
    std::vector<ReportBlock> blocks;
};

struct ReceiverReport {
    std::uint32_t ssrc = 0;
    std::vector<ReportBlock> blocks;
};

using RtcpPacket = std::variant<SenderReport, ReceiverReport>;

std::vector<std::uint8_t> serialize(const SenderReport& sr);
std::vector<std::uint8_t> serialize(const ReceiverReport& rr);
RtcpPacket parseRtcp(const std::vector<std::uint8_t>& data);

// Virtual time 0 is mapped to 2026-01-01T00:00:00Z in the NTP timescale.
constexpr std::uint64_t kNtpSecondsAtSimEpoch = 3'976'214'400ULL;
std::uint64_t toNtp(Micros t);
Micros fromNtp(std::uint64_t ntp);
inline std::uint32_t ntpMiddle32(std::uint64_t ntp) { return static_cast<std::uint32_t>(ntp >> 16); }

// RTT = A - LSR - DLSR, all in 1/65536 s (RFC 3550 section 6.4.1).
double roundTripMs(std::uint32_t arrivalMiddle32, std::uint32_t lsr, std::uint32_t dlsr);

// Maps a stream's RTP timestamps to the sender's wallclock using the latest SR.
class ClockMapping {
public:
    explicit ClockMapping(int clockRate) : rate_(clockRate) {}
    void update(std::uint64_t ntp, std::uint32_t rtpTimestamp) {
        ntp_ = ntp;
        rtp_ = rtpTimestamp;
        valid_ = true;
    }
    bool valid() const { return valid_; }
    Micros wallclock(std::uint32_t rtpTimestamp) const {
        std::int64_t diff = static_cast<std::int32_t>(rtpTimestamp - rtp_);  // handles wrap
        return fromNtp(ntp_) + diff * 1'000'000 / rate_;
    }

private:
    int rate_;
    std::uint64_t ntp_ = 0;
    std::uint32_t rtp_ = 0;
    bool valid_ = false;
};

// Measures how much later video is presented than audio captured at the same instant.
// For each played frame, end-to-end delay = local playout time - sender capture wallclock
// (from the SR mapping). The offset is the smoothed video delay minus the smoothed audio delay;
// a positive value means video lags and audio should be held back by that amount.
class LipSync {
public:
    LipSync(int audioRate, int videoRate) : audio_(audioRate), video_(videoRate) {}

    void onSenderReport(bool video, std::uint64_t ntp, std::uint32_t rtpTimestamp) {
        (video ? video_ : audio_).update(ntp, rtpTimestamp);
    }
    void onPlayout(bool video, std::uint32_t rtpTimestamp, Micros playedAt);
    std::optional<double> offsetMs() const;

private:
    ClockMapping audio_;
    ClockMapping video_;
    std::optional<double> audioDelayUs_;
    std::optional<double> videoDelayUs_;
};

}  // namespace svcc
