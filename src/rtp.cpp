#include "svcc/rtp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace svcc {

std::vector<std::uint8_t> RtpPacket::serialize() const {
    std::vector<std::uint8_t> out(12 + payload.size());
    out[0] = 0x80;  // V=2, no padding, no extension, CC=0
    out[1] = static_cast<std::uint8_t>((marker ? 0x80 : 0) | (payloadType & 0x7f));
    out[2] = static_cast<std::uint8_t>(seq >> 8);
    out[3] = static_cast<std::uint8_t>(seq);
    for (int i = 0; i < 4; ++i) {
        out[4 + i] = static_cast<std::uint8_t>(timestamp >> (24 - 8 * i));
        out[8 + i] = static_cast<std::uint8_t>(ssrc >> (24 - 8 * i));
    }
    std::copy(payload.begin(), payload.end(), out.begin() + 12);
    return out;
}

RtpPacket RtpPacket::parse(const std::vector<std::uint8_t>& d) {
    if (d.size() < 12) throw RtpError("RTP packet shorter than its header");
    if ((d[0] >> 6) != 2) throw RtpError("not RTP version 2");
    std::size_t header = 12 + 4u * (d[0] & 0x0f);
    if (d.size() < header) throw RtpError("truncated CSRC list");
    RtpPacket p;
    p.marker = (d[1] & 0x80) != 0;
    p.payloadType = d[1] & 0x7f;
    p.seq = static_cast<std::uint16_t>((d[2] << 8) | d[3]);
    p.timestamp = (std::uint32_t(d[4]) << 24) | (std::uint32_t(d[5]) << 16) | (std::uint32_t(d[6]) << 8) | d[7];
    p.ssrc = (std::uint32_t(d[8]) << 24) | (std::uint32_t(d[9]) << 16) | (std::uint32_t(d[10]) << 8) | d[11];
    p.payload.assign(d.begin() + static_cast<long>(header), d.end());
    return p;
}

bool looksLikeRtcp(const std::vector<std::uint8_t>& d) { return d.size() >= 8 && d[1] >= 200 && d[1] <= 204; }

std::vector<RtpPacket> Packetizer::packetize(std::uint32_t timestamp, std::size_t frameBytes, std::size_t maxPayload) {
    std::vector<RtpPacket> out;
    std::size_t left = frameBytes;
    do {
        std::size_t n = std::min(left, maxPayload);
        RtpPacket p;
        p.payloadType = pt_;
        p.seq = seq_++;
        p.timestamp = timestamp;
        p.ssrc = ssrc_;
        p.payload.assign(n, static_cast<std::uint8_t>(timestamp));
        left -= n;
        p.marker = left == 0;
        ++packets_;
        octets_ += static_cast<std::uint32_t>(n);
        out.push_back(std::move(p));
    } while (left > 0);
    return out;
}

// ------------------------------------------------------------------ receiver statistics

void ReceiverStats::onPacket(std::uint16_t seq, std::uint32_t rtpTimestamp, std::uint32_t arrival) {
    constexpr std::uint16_t kMaxDropout = 3000;
    constexpr std::uint16_t kMaxMisorder = 100;
    if (!initialized_) {
        initialized_ = true;
        baseSeq_ = maxSeq_ = seq;
    } else {
        std::uint16_t udelta = static_cast<std::uint16_t>(seq - maxSeq_);
        if (udelta < kMaxDropout) {
            if (seq < maxSeq_) cycles_ += 65536;  // sequence number wrapped
            maxSeq_ = seq;
        } else if (udelta <= 65535 - kMaxMisorder) {
            // a very large jump: treat as a restart of the source (simplified A.1 resync)
            baseSeq_ = maxSeq_ = seq;
            cycles_ = 0;
            received_ = expectedPrior_ = receivedPrior_ = 0;
        }
        // otherwise: duplicate or reordered packet, counted below
    }
    ++received_;

    // A.8: D(i,j) = (Rj - Ri) - (Sj - Si);  J += (|D| - J) / 16
    std::int64_t transit = static_cast<std::int64_t>(arrival) - static_cast<std::int64_t>(rtpTimestamp);
    if (haveTransit_) {
        double d = std::fabs(static_cast<double>(transit - lastTransit_));
        jitter_ += (d - jitter_) / 16.0;
    }
    lastTransit_ = transit;
    haveTransit_ = true;
}

std::int32_t ReceiverStats::cumulativeLost() const {
    if (!initialized_) return 0;
    std::int64_t expected = static_cast<std::int64_t>(extendedHighestSeq()) - baseSeq_ + 1;
    std::int64_t lost = expected - received_;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(lost, -0x800000, 0x7fffff));
}

std::uint8_t ReceiverStats::fractionLostSinceLastReport() {
    if (!initialized_) return 0;
    std::uint32_t expected = extendedHighestSeq() - baseSeq_ + 1;
    std::uint32_t expectedInterval = expected - expectedPrior_;
    std::uint32_t receivedInterval = received_ - receivedPrior_;
    expectedPrior_ = expected;
    receivedPrior_ = received_;
    std::int64_t lostInterval = static_cast<std::int64_t>(expectedInterval) - receivedInterval;
    if (expectedInterval == 0 || lostInterval <= 0) return 0;
    return static_cast<std::uint8_t>((lostInterval << 8) / expectedInterval);
}

// ------------------------------------------------------------------ jitter buffer

JitterBuffer::JitterBuffer(int clockRate, Micros minDelay, Micros maxDelay)
    : rate_(clockRate), min_(minDelay), max_(maxDelay), delay_(minDelay) {}

std::int64_t JitterBuffer::unwrapSeq(std::uint16_t seq) {
    if (lastSeqExt_ < 0) return lastSeqExt_ = seq;
    std::int64_t candidate = (lastSeqExt_ & ~0xffffLL) | seq;
    if (candidate < lastSeqExt_ - 32768) candidate += 65536;
    else if (candidate > lastSeqExt_ + 32768) candidate -= 65536;
    lastSeqExt_ = std::max(lastSeqExt_, candidate);
    return candidate;
}

std::int64_t JitterBuffer::unwrapTs(std::uint32_t ts) {
    if (lastTsExt_ < 0) return lastTsExt_ = ts;
    std::int64_t candidate = (lastTsExt_ & ~0xffffffffLL) | ts;
    if (candidate < lastTsExt_ - 0x80000000LL) candidate += 0x100000000LL;
    else if (candidate > lastTsExt_ + 0x80000000LL) candidate -= 0x100000000LL;
    lastTsExt_ = std::max(lastTsExt_, candidate);
    return candidate;
}

Micros JitterBuffer::playoutTime(std::int64_t ts) const {
    return anchor_ + (ts - ts0_) * 1'000'000 / rate_ + delay_ + extra_;
}

void JitterBuffer::push(const RtpPacket& p, Micros arrival) {
    ++stats_.packets;
    std::int64_t seq = unwrapSeq(p.seq);
    std::int64_t ts = unwrapTs(p.timestamp);
    if (!started_) {
        started_ = true;
        ts0_ = ts;
        anchor_ = arrival;
    }
    // jitter estimate in microseconds, same smoothing as RFC 3550 A.8
    double transit = static_cast<double>(arrival) - static_cast<double>(ts - ts0_) * 1e6 / rate_;
    if (havePrevTransit_) jitterUs_ += (std::fabs(transit - prevTransitUs_) - jitterUs_) / 16.0;
    prevTransitUs_ = transit;
    havePrevTransit_ = true;

    if (ts <= lastPlayedTs_ || seq <= lastPlayedSeq_) {
        ++stats_.late;
        return;
    }
    auto it = frames_.find(ts);
    if (it == frames_.end()) {
        Frame fresh;
        fresh.firstSeq = fresh.lastSeq = seq;
        it = frames_.emplace(ts, fresh).first;
    }
    Frame& f = it->second;
    if (f.seqs.count(seq)) {
        ++stats_.duplicates;
        return;
    }
    f.seqs[seq] = true;
    f.firstSeq = std::min(f.firstSeq, seq);
    f.lastSeq = std::max(f.lastSeq, seq);
    ++f.count;
    f.marker = f.marker || p.marker;
}

std::vector<PlayoutFrame> JitterBuffer::pop(Micros now) {
    std::vector<PlayoutFrame> out;
    while (!frames_.empty()) {
        auto it = frames_.begin();
        Micros at = playoutTime(it->first);
        if (at > now) break;
        Frame& f = it->second;
        bool complete = f.marker && f.count == f.lastSeq - f.firstSeq + 1;
        if (lastPlayedSeq_ >= 0 && f.firstSeq > lastPlayedSeq_ + 1) {
            stats_.packetsConcealed += static_cast<std::uint64_t>(f.firstSeq - lastPlayedSeq_ - 1);
            complete = false;  // the gap may belong to this frame
        }
        stats_.packetsConcealed += static_cast<std::uint64_t>(f.lastSeq - f.firstSeq + 1 - f.count);
        ++stats_.framesPlayed;
        if (!complete) ++stats_.framesIncomplete;
        out.push_back({static_cast<std::uint32_t>(it->first), at, f.count, complete});
        lastPlayedTs_ = it->first;
        lastPlayedSeq_ = std::max(lastPlayedSeq_, f.lastSeq);
        frames_.erase(it);

        // adapt between frames, in steps of at most 2 ms
        Micros target = std::clamp<Micros>(static_cast<Micros>(3 * jitterUs_) + msec(10), min_, max_);
        Micros step = std::clamp<Micros>(target - delay_, -msec(2), msec(2));
        delay_ += step;
    }
    return out;
}

}  // namespace svcc
