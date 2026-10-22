#include "svcc/lab.hpp"
#include "svcc/sdp.hpp"

#include <gtest/gtest.h>

using namespace svcc;

namespace {

struct MatrixCase {
    const char* name;
    Capabilities offerer;
    Capabilities answerer;
    bool offerVideo;
    const char* audio;  // expected codec, "" = rejected
    const char* video;  // expected codec, "" = no active video
    std::size_t mlines;
};

std::string codecOf(const std::vector<NegotiatedStream>& s, const std::string& type) {
    for (const auto& x : s)
        if (x.type == type && x.active) return x.codec.name;
    return "";
}

}  // namespace

TEST(SdpNegotiation, Matrix) {
    using namespace codecs;
    const std::vector<MatrixCase> cases = {
        {"both video, same preference", {{PCMU, PCMA}, {H263, H261}}, {{PCMU, PCMA}, {H263, H261}}, true, "PCMU", "H263", 2},
        {"offerer order wins", {{PCMA, PCMU}, {H261, H263}}, {{PCMU, PCMA}, {H263, H261}}, true, "PCMA", "H261", 2},
        {"answerer subset", {{PCMU, PCMA}, {H261, H263}}, {{PCMA}, {H263}}, true, "PCMA", "H263", 2},
        {"answerer has no video", {{PCMU, PCMA}, {H263}}, {{PCMA}, {}}, true, "PCMA", "", 2},
        {"audio-only offer to video answerer", {{PCMU}, {H263}}, {{PCMU}, {H263}}, false, "PCMU", "", 1},
        {"no common video codec", {{PCMU}, {H261}}, {{PCMU}, {H263}}, true, "PCMU", "", 2},
        {"no common audio codec", {{PCMU}, {}}, {{PCMA}, {}}, false, "", "", 1},
    };
    for (const auto& c : cases) {
        SessionDescription offer = makeOffer(c.offerer, "a", {5000, 5002}, c.offerVideo);
        SessionDescription answer = makeAnswer(offer, c.answerer, "b", {6000, 6002});
        // the answer travels as text, like it does in a 200 OK
        answer = SessionDescription::parse(answer.toString());
        ASSERT_EQ(answer.media.size(), c.mlines) << c.name;
        for (std::size_t i = 0; i < answer.media.size(); ++i) EXPECT_EQ(answer.media[i].type, offer.media[i].type) << c.name;
        auto atOfferer = negotiate(offer, answer, answer);
        auto atAnswerer = negotiate(answer, offer, answer);
        EXPECT_EQ(codecOf(atOfferer, "audio"), c.audio) << c.name;
        EXPECT_EQ(codecOf(atOfferer, "video"), c.video) << c.name;
        EXPECT_EQ(codecOf(atAnswerer, "audio"), c.audio) << c.name << " (answerer view)";
        EXPECT_EQ(codecOf(atAnswerer, "video"), c.video) << c.name << " (answerer view)";
        for (const auto& m : answer.media)
            if (!m.active()) EXPECT_EQ(m.payloads.size(), 1u) << c.name << ": a rejected stream still lists a format";
    }
}

TEST(SdpNegotiation, DirectionsForHold) {
    EXPECT_EQ(answerDirection(Direction::SendOnly, false), Direction::RecvOnly);
    EXPECT_EQ(answerDirection(Direction::RecvOnly, false), Direction::SendOnly);
    EXPECT_EQ(answerDirection(Direction::Inactive, false), Direction::Inactive);
    EXPECT_EQ(answerDirection(Direction::SendRecv, false), Direction::SendRecv);
    EXPECT_EQ(answerDirection(Direction::SendRecv, true), Direction::SendOnly);

    Capabilities caps{{codecs::PCMU}, {}};
    SessionDescription hold = makeOffer(caps, "a", {5000, 0}, false, Direction::SendOnly);
    SessionDescription answer = makeAnswer(hold, caps, "b", {6000, 0});
    auto atHolder = negotiate(hold, answer, answer);
    auto atHeld = negotiate(answer, hold, answer);
    EXPECT_TRUE(sends(atHolder[0].localDir));
    EXPECT_FALSE(receives(atHolder[0].localDir));
    EXPECT_FALSE(sends(atHeld[0].localDir));
}

TEST(SdpNegotiation, ReofferKeepsMlinesAndBumpsVersion) {
    Capabilities caps{{codecs::PCMU}, {codecs::H263}};
    SessionDescription first = makeOffer(caps, "a", {5000, 5002}, true);
    SessionDescription noVideo = makeReoffer(first, caps, {5000, 5002}, false, Direction::SendRecv);
    ASSERT_EQ(noVideo.media.size(), 2u);
    EXPECT_EQ(noVideo.version, first.version + 1);
    EXPECT_EQ(noVideo.media[1].port, 0);
    SessionDescription again = makeReoffer(noVideo, caps, {5000, 5002}, true, Direction::SendRecv);
    ASSERT_EQ(again.media.size(), 2u);
    EXPECT_EQ(again.media[1].port, 5002);

    SessionDescription audioOnly = makeOffer(caps, "a", {5000, 5002}, false);
    SessionDescription added = makeReoffer(audioOnly, caps, {5000, 5002}, true, Direction::SendRecv);
    ASSERT_EQ(added.media.size(), 2u);
    EXPECT_EQ(added.media[1].type, "video");
}

TEST(SdpParse, RoundTripAndErrors) {
    SessionDescription s = makeOffer({{codecs::PCMU, codecs::PCMA}, {codecs::H263}}, "host-a", {4000, 4002}, true,
                                     Direction::RecvOnly);
    SessionDescription back = SessionDescription::parse(s.toString());
    EXPECT_EQ(back.address, "host-a");
    ASSERT_EQ(back.media.size(), 2u);
    EXPECT_EQ(back.media[0].payloads, (std::vector<int>{0, 8}));
    EXPECT_EQ(back.media[1].payloads, (std::vector<int>{34}));
    EXPECT_EQ(back.media[1].dir, Direction::RecvOnly);
    EXPECT_THROW(SessionDescription::parse("o=- 1 1 IN IP4 x\r\n"), SdpError);
    EXPECT_THROW(SessionDescription::parse("v=0\r\nm=audio x RTP/AVP 0\r\n"), SdpError);
    EXPECT_THROW(SessionDescription::parse("v=0\r\nm=audio 4000 RTP/SAVP 0\r\n"), SdpError);
    EXPECT_THROW(SessionDescription::parse("v=0\r\nm=audio 4000 RTP/AVP\r\n"), SdpError);
    EXPECT_THROW(SessionDescription::parse(""), SdpError);
}

// Two phones talking directly: re-INVITE removes and restores video, and hold changes direction.
TEST(ReInvite, AddRemoveVideoAndHoldBetweenPhones) {
    Scheduler sched;
    SipNetwork sip(sched, msec(1));
    MediaNetwork net(sched, 1);
    SipPhone a(sched, sip, net, "host-a", "alice", videoPhoneCaps(), 10000);
    SipPhone b(sched, sip, net, "host-b", "bob", videoPhoneCaps(), 12000);
    b.autoAnswer = true;
    sip.attach("host-a", &a);
    sip.attach("host-b", &b);

    std::string id = a.call("sip:bob@host-b", "host-b", true);
    sched.runUntil(msec(200));
    ASSERT_TRUE(a.videoActive(id));
    ASSERT_TRUE(b.videoActive(id));

    a.reinvite(id, false, false);
    sched.runUntil(msec(400));
    EXPECT_FALSE(a.videoActive(id));
    EXPECT_FALSE(b.videoActive(id));
    auto sentBefore = b.find(id)->media->video().report().packetsSent;
    sched.runUntil(msec(1400));
    EXPECT_EQ(b.find(id)->media->video().report().packetsSent, sentBefore) << "video stopped";

    a.reinvite(id, true, false);
    sched.runUntil(msec(1600));
    EXPECT_TRUE(a.videoActive(id));
    EXPECT_TRUE(b.videoActive(id));

    a.reinvite(id, true, true);
    sched.runUntil(msec(1800));
    const auto* held = b.find(id);
    ASSERT_NE(held, nullptr);
    for (const auto& s : held->streams)
        if (s.active) EXPECT_EQ(s.localDir, Direction::RecvOnly);
    EXPECT_EQ(held->remote.version, 4u) << "each re-offer increments the version";
}
