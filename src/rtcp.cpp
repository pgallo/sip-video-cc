#include "svcc/rtcp.hpp"

namespace svcc {

namespace {

void put32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint32_t get32(const std::vector<std::uint8_t>& d, std::size_t at) {
    return (std::uint32_t(d[at]) << 24) | (std::uint32_t(d[at + 1]) << 16) | (std::uint32_t(d[at + 2]) << 8) |
           d[at + 3];
}

void putHeader(std::vector<std::uint8_t>& o, int count, int type, std::size_t words) {
    o.push_back(static_cast<std::uint8_t>(0x80 | count));
    o.push_back(static_cast<std::uint8_t>(type));
    o.push_back(static_cast<std::uint8_t>((words - 1) >> 8));  // length in 32-bit words minus one
    o.push_back(static_cast<std::uint8_t>(words - 1));
}

void putBlock(std::vector<std::uint8_t>& o, const ReportBlock& b) {
    put32(o, b.ssrc);
    std::uint32_t lost = static_cast<std::uint32_t>(b.cumulativeLost) & 0xffffff;
    put32(o, (std::uint32_t(b.fractionLost) << 24) | lost);
    put32(o, b.extendedHighestSeq);
    put32(o, b.jitter);
    put32(o, b.lastSr);
    put32(o, b.delaySinceLastSr);
}

ReportBlock getBlock(const std::vector<std::uint8_t>& d, std::size_t at) {
    ReportBlock b;
    b.ssrc = get32(d, at);
    std::uint32_t w = get32(d, at + 4);
    b.fractionLost = static_cast<std::uint8_t>(w >> 24);
    std::int32_t lost = static_cast<std::int32_t>(w & 0xffffff);
    if (lost & 0x800000) lost -= 0x1000000;  // sign-extend 24 bits
    b.cumulativeLost = lost;
    b.extendedHighestSeq = get32(d, at + 8);
    b.jitter = get32(d, at + 12);
    b.lastSr = get32(d, at + 16);
    b.delaySinceLastSr = get32(d, at + 20);
    return b;
}

}  // namespace

std::vector<std::uint8_t> serialize(const SenderReport& sr) {
    std::vector<std::uint8_t> o;
    putHeader(o, static_cast<int>(sr.blocks.size()), 200, 7 + 6 * sr.blocks.size());
    put32(o, sr.ssrc);
    put32(o, static_cast<std::uint32_t>(sr.ntp >> 32));
    put32(o, static_cast<std::uint32_t>(sr.ntp));
    put32(o, sr.rtpTimestamp);
    put32(o, sr.packetCount);
    put32(o, sr.octetCount);
    for (const auto& b : sr.blocks) putBlock(o, b);
    return o;
}

std::vector<std::uint8_t> serialize(const ReceiverReport& rr) {
    std::vector<std::uint8_t> o;
    putHeader(o, static_cast<int>(rr.blocks.size()), 201, 2 + 6 * rr.blocks.size());
    put32(o, rr.ssrc);
    for (const auto& b : rr.blocks) putBlock(o, b);
    return o;
}

RtcpPacket parseRtcp(const std::vector<std::uint8_t>& d) {
    if (d.size() < 8 || (d[0] >> 6) != 2) throw RtcpError("not an RTCP packet");
    int count = d[0] & 0x1f;
    int type = d[1];
    std::size_t bytes = (((std::size_t(d[2]) << 8) | d[3]) + 1) * 4;
    if (bytes > d.size()) throw RtcpError("RTCP length exceeds datagram");
    std::size_t fixed = type == 200 ? 28 : 8;
    if (type != 200 && type != 201) throw RtcpError("unsupported RTCP type " + std::to_string(type));
    if (bytes < fixed + 24u * count) throw RtcpError("RTCP report blocks truncated");
    if (type == 200) {
        SenderReport sr;
        sr.ssrc = get32(d, 4);
        sr.ntp = (std::uint64_t(get32(d, 8)) << 32) | get32(d, 12);
        sr.rtpTimestamp = get32(d, 16);
        sr.packetCount = get32(d, 20);
        sr.octetCount = get32(d, 24);
        for (int i = 0; i < count; ++i) sr.blocks.push_back(getBlock(d, 28 + 24 * i));
        return sr;
    }
    ReceiverReport rr;
    rr.ssrc = get32(d, 4);
    for (int i = 0; i < count; ++i) rr.blocks.push_back(getBlock(d, 8 + 24 * i));
    return rr;
}

std::uint64_t toNtp(Micros t) {
    std::uint64_t secs = kNtpSecondsAtSimEpoch + static_cast<std::uint64_t>(t / 1'000'000);
    std::uint64_t frac = (static_cast<std::uint64_t>(t % 1'000'000) << 32) / 1'000'000;
    return (secs << 32) | frac;
}

Micros fromNtp(std::uint64_t ntp) {
    std::int64_t secs = static_cast<std::int64_t>(ntp >> 32) - static_cast<std::int64_t>(kNtpSecondsAtSimEpoch);
    std::uint64_t frac = ntp & 0xffffffffULL;
    return secs * 1'000'000 + static_cast<Micros>((frac * 1'000'000 + 0x80000000ULL) >> 32);
}

double roundTripMs(std::uint32_t arrivalMiddle32, std::uint32_t lsr, std::uint32_t dlsr) {
    if (lsr == 0) return 0;
    std::uint32_t rtt = arrivalMiddle32 - lsr - dlsr;
    return rtt * 1000.0 / 65536.0;
}

void LipSync::onPlayout(bool video, std::uint32_t rtpTimestamp, Micros playedAt) {
    const ClockMapping& map = video ? video_ : audio_;
    if (!map.valid()) return;
    double delay = static_cast<double>(playedAt - map.wallclock(rtpTimestamp));
    auto& slot = video ? videoDelayUs_ : audioDelayUs_;
    slot = slot ? *slot + (delay - *slot) / 8.0 : delay;
}

std::optional<double> LipSync::offsetMs() const {
    if (!audioDelayUs_ || !videoDelayUs_) return std::nullopt;
    return (*videoDelayUs_ - *audioDelayUs_) / 1000.0;
}

}  // namespace svcc
