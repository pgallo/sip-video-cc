#include "svcc/rtp.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace svcc;

TEST(Rtp, SerializeParseRoundTrip) {
    RtpPacket p;
    p.marker = true;
    p.payloadType = 34;
    p.seq = 65535;
    p.timestamp = 0xdeadbeef;
    p.ssrc = 0x01020304;
    p.payload = {1, 2, 3};
    auto bytes = p.serialize();
    ASSERT_EQ(bytes.size(), 15u);
    RtpPacket q = RtpPacket::parse(bytes);
    EXPECT_TRUE(q.marker);
    EXPECT_EQ(q.payloadType, 34);
    EXPECT_EQ(q.seq, 65535);
    EXPECT_EQ(q.timestamp, 0xdeadbeefu);
    EXPECT_EQ(q.ssrc, 0x01020304u);
    EXPECT_EQ(q.payload, p.payload);
    EXPECT_FALSE(looksLikeRtcp(bytes));
    EXPECT_THROW(RtpPacket::parse({0x80, 0}), RtpError);
    EXPECT_THROW(RtpPacket::parse(std::vector<std::uint8_t>(12, 0x40)), RtpError);
}

TEST(Rtp, PacketizerSplitsFramesAndMarksTheLastPacket) {
    Packetizer pz(7, 34, 100);
    auto pkts = pz.packetize(9000, 3000, 1200);
    ASSERT_EQ(pkts.size(), 3u);
    EXPECT_EQ(pkts[0].payload.size(), 1200u);
    EXPECT_EQ(pkts[2].payload.size(), 600u);
    EXPECT_FALSE(pkts[0].marker);
    EXPECT_FALSE(pkts[1].marker);
    EXPECT_TRUE(pkts[2].marker);
    EXPECT_EQ(pkts[1].seq, 101);
    for (const auto& p : pkts) EXPECT_EQ(p.timestamp, 9000u);
    EXPECT_EQ(pz.packetsSent(), 3u);
    EXPECT_EQ(pz.octetsSent(), 3000u);
}

TEST(ReceiverStats, JitterFollowsRfc3550Formula) {
    // transit alternates 0, 10, 0, 10 ... timestamp units, so |D| = 10 for every pair
    ReceiverStats rs;
    double expected = 0;
    for (int i = 0; i < 50; ++i) {
        std::uint32_t ts = 1000 + 160 * i;
        rs.onPacket(static_cast<std::uint16_t>(i), ts, ts + (i % 2 ? 10 : 0));
        if (i > 0) expected += (10.0 - expected) / 16.0;
    }
    EXPECT_DOUBLE_EQ(rs.jitterExact(), expected);
    EXPECT_NEAR(rs.jitterExact(), 10.0 * (1 - std::pow(15.0 / 16.0, 49)), 1e-9);

    ReceiverStats two;
    two.onPacket(1, 0, 100);
    two.onPacket(2, 160, 270);  // D = (270 - 100) - (160 - 0) = 10
    EXPECT_DOUBLE_EQ(two.jitterExact(), 10.0 / 16.0);
}

TEST(ReceiverStats, LossFractionAndSequenceWrap) {
    ReceiverStats rs;
    std::uint16_t seq = 65500;
    int received = 0;
    for (int i = 0; i < 100; ++i, ++seq) {
        if (i >= 40 && i < 45) continue;  // five lost, across the wrap at 65535
        rs.onPacket(seq, 160u * i, 160u * i);
        ++received;
    }
    EXPECT_EQ(rs.received(), static_cast<std::uint32_t>(received));
    EXPECT_EQ(rs.cumulativeLost(), 5);
    EXPECT_EQ(rs.extendedHighestSeq(), 65536u + 63u);
    EXPECT_EQ(rs.fractionLostSinceLastReport(), (5 * 256) / 100);
    EXPECT_EQ(rs.fractionLostSinceLastReport(), 0) << "no new packets: nothing lost in the interval";
}

namespace {
RtpPacket audio(std::uint16_t seq, std::uint32_t ts) {
    RtpPacket p;
    p.payloadType = 0;
    p.seq = seq;
    p.timestamp = ts;
    p.marker = true;
    return p;
}
}  // namespace

TEST(JitterBuffer, PlaysInOrderAfterTheDelayAndReordersWithinIt) {
    JitterBuffer jb(8000, msec(40), msec(200));
    jb.push(audio(1, 0), 0);
    jb.push(audio(3, 320), msec(41));  // arrives before seq 2
    jb.push(audio(2, 160), msec(45));
    EXPECT_TRUE(jb.pop(msec(39)).empty());
    auto out = jb.pop(msec(100));
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].timestamp, 0u);
    EXPECT_EQ(out[1].timestamp, 160u);
    EXPECT_EQ(out[2].timestamp, 320u);
    EXPECT_EQ(out[0].playoutAt, msec(40));
    EXPECT_EQ(jb.stats().packetsConcealed, 0u);
}

TEST(JitterBuffer, CountsLateLostAndIncompleteFrames) {
    JitterBuffer jb(8000, msec(20), msec(200));
    jb.push(audio(1, 0), 0);
    jb.push(audio(2, 160), msec(20));
    jb.push(audio(4, 480), msec(60));  // seq 3 never arrives
    auto out = jb.pop(msec(200));
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(jb.stats().packetsConcealed, 1u);
    EXPECT_FALSE(out[2].complete);
    jb.push(audio(3, 320), msec(210));  // too late: its slot has been played
    EXPECT_EQ(jb.stats().late, 1u);
    jb.push(audio(4, 480), msec(211));
    EXPECT_EQ(jb.stats().late, 2u);

    // a video frame of three packets with the middle one missing is incomplete
    JitterBuffer vb(90000, msec(40), msec(200));
    for (std::uint16_t s : {10, 12}) {
        RtpPacket p;
        p.payloadType = 34;
        p.seq = s;
        p.timestamp = 9000;
        p.marker = s == 12;
        vb.push(p, 0);
    }
    auto frames = vb.pop(msec(100));
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_FALSE(frames[0].complete);
    EXPECT_EQ(frames[0].packets, 2);
    EXPECT_EQ(vb.stats().packetsConcealed, 1u);
}

TEST(JitterBuffer, DelayAdaptsToJitter) {
    JitterBuffer jb(8000, msec(20), msec(300));
    // arrival jitter alternating 0 and 30 ms
    for (int i = 0; i < 400; ++i) {
        jb.push(audio(static_cast<std::uint16_t>(i), 160u * i), msec(20) * i + (i % 2 ? msec(30) : 0));
        jb.pop(msec(20) * i);
    }
    EXPECT_GT(jb.jitterMicros(), 20000.0);
    EXPECT_GT(jb.currentDelay(), msec(60));
    EXPECT_LE(jb.currentDelay(), msec(300));
}
