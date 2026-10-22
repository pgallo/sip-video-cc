// RTP (RFC 3550): packet format, packetisation of synthetic frames, receiver statistics
// (sequence tracking, loss and interarrival jitter as in Appendix A) and an adaptive jitter
// buffer with loss-concealment accounting.
#pragma once

#include "svcc/sim.hpp"

#include <cstdint>
#include <map>
#include <stdexcept>
#include <vector>

namespace svcc {

struct RtpError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct RtpPacket {
    bool marker = false;
    std::uint8_t payloadType = 0;
    std::uint16_t seq = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t ssrc = 0;
    std::vector<std::uint8_t> payload;

    std::vector<std::uint8_t> serialize() const;
    static RtpPacket parse(const std::vector<std::uint8_t>& data);
};

// True for RTCP packet types 200..204 (SR, RR, SDES, BYE, APP), used to demultiplex.
bool looksLikeRtcp(const std::vector<std::uint8_t>& data);

// Splits a frame into packets of at most maxPayload bytes; the marker bit is set on the last
// packet of the frame (end of a video frame, RFC 3551 convention).
class Packetizer {
public:
    Packetizer(std::uint32_t ssrc, std::uint8_t payloadType, std::uint16_t firstSeq)
        : ssrc_(ssrc), pt_(payloadType), seq_(firstSeq) {}

    std::vector<RtpPacket> packetize(std::uint32_t timestamp, std::size_t frameBytes, std::size_t maxPayload);
    std::uint32_t packetsSent() const { return packets_; }
    std::uint32_t octetsSent() const { return octets_; }

private:
    std::uint32_t ssrc_;
    std::uint8_t pt_;
    std::uint16_t seq_;
    std::uint32_t packets_ = 0;
    std::uint32_t octets_ = 0;
};

// Receiver-side statistics of RFC 3550 Appendix A.1 (sequence validation), A.3 (loss) and
// A.8 (interarrival jitter).
class ReceiverStats {
public:
    // arrival is expressed in RTP timestamp units of the stream's clock.
    void onPacket(std::uint16_t seq, std::uint32_t rtpTimestamp, std::uint32_t arrival);

    std::uint32_t received() const { return received_; }
    std::uint32_t extendedHighestSeq() const { return cycles_ + maxSeq_; }
    std::int32_t cumulativeLost() const;
    std::uint32_t jitter() const { return static_cast<std::uint32_t>(jitter_); }
    double jitterExact() const { return jitter_; }
    // Fraction lost since the previous call, as the 8-bit fixed-point value carried in reports.
    std::uint8_t fractionLostSinceLastReport();
    bool started() const { return initialized_; }

private:
    bool initialized_ = false;
    std::uint16_t baseSeq_ = 0;
    std::uint16_t maxSeq_ = 0;
    std::uint32_t cycles_ = 0;
    std::uint32_t received_ = 0;
    std::uint32_t expectedPrior_ = 0;
    std::uint32_t receivedPrior_ = 0;
    bool haveTransit_ = false;
    std::int64_t lastTransit_ = 0;
    double jitter_ = 0;
};

struct PlayoutFrame {
    std::uint32_t timestamp;
    Micros playoutAt;
    int packets;
    bool complete;
};

struct JitterStats {
    std::uint64_t packets = 0;
    std::uint64_t late = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t framesPlayed = 0;
    std::uint64_t framesIncomplete = 0;
    std::uint64_t packetsConcealed = 0;
};

// Adaptive playout buffer. Packets are grouped into frames by timestamp; a frame is played at
//   playout(ts) = anchor + (ts - ts0) / clockRate + delay
// where delay tracks max(minDelay, 3 * jitter + headroom) and is moved by small steps only
// between frames. Sequence gaps found at playout time are counted as concealed packets, and a
// frame with missing packets or no marker bit is reported as incomplete.
class JitterBuffer {
public:
    JitterBuffer(int clockRate, Micros minDelay, Micros maxDelay);

    void push(const RtpPacket& p, Micros arrival);
    std::vector<PlayoutFrame> pop(Micros now);

    // Additional delay requested by lip synchronisation.
    void setExtraDelay(Micros d) { extra_ = d; }
    Micros extraDelay() const { return extra_; }
    Micros currentDelay() const { return delay_; }
    double jitterMicros() const { return jitterUs_; }
    const JitterStats& stats() const { return stats_; }

private:
    struct Frame {
        std::int64_t firstSeq = 0;
        std::int64_t lastSeq = 0;
        int count = 0;
        bool marker = false;
        std::map<std::int64_t, bool> seqs;
    };

    std::int64_t unwrapSeq(std::uint16_t seq);
    std::int64_t unwrapTs(std::uint32_t ts);
    Micros playoutTime(std::int64_t ts) const;

    int rate_;
    Micros min_;
    Micros max_;
    Micros delay_;
    Micros extra_ = 0;
    bool started_ = false;
    std::int64_t ts0_ = 0;
    Micros anchor_ = 0;
    std::int64_t lastSeqExt_ = -1;
    std::int64_t lastTsExt_ = -1;
    std::int64_t lastPlayedTs_ = -1;
    std::int64_t lastPlayedSeq_ = -1;
    bool havePrevTransit_ = false;
    double prevTransitUs_ = 0;
    double jitterUs_ = 0;
    std::map<std::int64_t, Frame> frames_;
    JitterStats stats_;
};

}  // namespace svcc
