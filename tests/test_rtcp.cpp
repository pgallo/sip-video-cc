#include "svcc/media.hpp"
#include "svcc/rtcp.hpp"

#include <gtest/gtest.h>

using namespace svcc;

TEST(Rtcp, SenderReportRoundTripWithSignedLoss) {
    SenderReport sr;
    sr.ssrc = 0x11223344;
    sr.ntp = toNtp(msec(1234));
    sr.rtpTimestamp = 987654;
    sr.packetCount = 50;
    sr.octetCount = 8000;
    ReportBlock b1;
    b1.ssrc = 9;
    b1.fractionLost = 25;
    b1.cumulativeLost = 17;
    b1.extendedHighestSeq = 70000;
    b1.jitter = 33;
    b1.lastSr = 0xabcd1234;
    b1.delaySinceLastSr = 65536;
    ReportBlock b2 = b1;
    b2.ssrc = 10;
    b2.cumulativeLost = -3;  // duplicates can make it negative
    sr.blocks = {b1, b2};
    auto bytes = serialize(sr);
    EXPECT_EQ(bytes.size(), 28u + 2 * 24u);
    EXPECT_TRUE(looksLikeRtcp(bytes));
    auto parsed = std::get<SenderReport>(parseRtcp(bytes));
    EXPECT_EQ(parsed.ssrc, sr.ssrc);
    EXPECT_EQ(parsed.ntp, sr.ntp);
    EXPECT_EQ(parsed.rtpTimestamp, sr.rtpTimestamp);
    EXPECT_EQ(parsed.packetCount, 50u);
    ASSERT_EQ(parsed.blocks.size(), 2u);
    EXPECT_EQ(parsed.blocks[0].fractionLost, 25);
    EXPECT_EQ(parsed.blocks[0].cumulativeLost, 17);
    EXPECT_EQ(parsed.blocks[1].cumulativeLost, -3);
    EXPECT_EQ(parsed.blocks[0].lastSr, 0xabcd1234u);
}

TEST(Rtcp, ReceiverReportAndMalformedInput) {
    ReceiverReport rr;
    rr.ssrc = 5;
    rr.blocks.push_back(ReportBlock{7, 0, 0, 100, 2, 0, 0});
    auto bytes = serialize(rr);
    auto parsed = std::get<ReceiverReport>(parseRtcp(bytes));
    EXPECT_EQ(parsed.blocks.at(0).extendedHighestSeq, 100u);

    auto truncated = bytes;
    truncated.resize(20);
    EXPECT_THROW(parseRtcp(truncated), RtcpError);
    auto wrongType = bytes;
    wrongType[1] = 203;  // BYE is not handled here
    EXPECT_THROW(parseRtcp(wrongType), RtcpError);
}

TEST(Rtcp, NtpConversions) {
    EXPECT_EQ(toNtp(0) >> 32, kNtpSecondsAtSimEpoch);
    for (Micros t : {Micros(0), Micros(1), msec(500), msec(61'234), Micros(3'600'000'001)}) EXPECT_EQ(fromNtp(toNtp(t)), t);
    EXPECT_EQ(ntpMiddle32(toNtp(msec(1500))) & 0xffff, 0x8000u) << "half a second in 1/65536 units";
}

TEST(Rtcp, RoundTripTimeFromLsrAndDlsr) {
    // SR sent at 1.000 s, received at 1.020 s, RR sent back 0.5 s later, arrives at 1.540 s
    std::uint32_t lsr = ntpMiddle32(toNtp(msec(1000)));
    std::uint32_t dlsr = 65536 / 2;
    double rtt = roundTripMs(ntpMiddle32(toNtp(msec(1540))), lsr, dlsr);
    EXPECT_NEAR(rtt, 40.0, 0.05);
    EXPECT_EQ(roundTripMs(123, 0, 0), 0.0) << "no SR received yet";
}

TEST(LipSync, OffsetIsVideoDelayMinusAudioDelay) {
    LipSync ls(8000, 90000);
    EXPECT_FALSE(ls.offsetMs().has_value());
    ls.onSenderReport(false, toNtp(msec(1000)), 1000);
    ls.onSenderReport(true, toNtp(msec(1000)), 90000);
    // a frame captured at 1.2 s in both streams: audio played 60 ms later, video 110 ms later
    ls.onPlayout(false, 1000 + 1600, msec(1260));
    ls.onPlayout(true, 90000 + 18000, msec(1310));
    ASSERT_TRUE(ls.offsetMs().has_value());
    EXPECT_NEAR(*ls.offsetMs(), 50.0, 0.01);
}

TEST(MediaStream, RtcpReportsLossAndRttBetweenTwoEndpoints) {
    Scheduler sched;
    MediaNetwork net(sched, 3);
    net.setProfile("a", "b", {msec(15), 0, 0.05, 0});
    net.setProfile("b", "a", {msec(15), 0, 0.0, 0});
    MediaStream a(sched, net, {"a", 4000}, "audio", 0xA);
    MediaStream b(sched, net, {"b", 6000}, "audio", 0xB);
    NegotiatedStream toB{"audio", true, codecs::PCMU, 6000, "b", Direction::SendRecv};
    NegotiatedStream toA{"audio", true, codecs::PCMU, 4000, "a", Direction::SendRecv};
    a.start(toB);
    b.start(toA);
    sched.runUntil(msec(20000));
    StreamReport atB = b.report();
    StreamReport atA = a.report();
    double lossAtB = static_cast<double>(atB.cumulativeLost) / (atB.packetsReceived + atB.cumulativeLost);
    EXPECT_NEAR(lossAtB, 0.05, 0.02);
    EXPECT_EQ(atA.cumulativeLost, 0);
    EXPECT_GT(atA.remoteCumulativeLost, 0) << "A learns about its loss from B's reports";
    EXPECT_NEAR(atA.rttMs, 30.0, 1.0);
    EXPECT_NEAR(atB.rttMs, 30.0, 1.0);
    EXPECT_GT(atB.buffer.framesPlayed, 900u);
}
