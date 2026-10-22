// End to end in one process: SIP phones, the B2BUA with media relay, agent desktops speaking
// CSTA over TCP, and an agent on the simulated commercial PBX reached through its CSTA link.
#include "svcc/lab.hpp"

#include <gtest/gtest.h>

#include <cmath>

using namespace svcc;

TEST(EndToEnd, VideoCallAnsweredViaCstaThenTransferredToPbxAgent) {
    Lab lab(5, msec(5000));
    lab.net.setProfile("caller", "cc", {msec(20), msec(20), 0.02, msec(50)});
    lab.net.setProfile("cc", "caller", {msec(20), msec(20), 0.02, msec(50)});
    lab.cc.addAgent({"2001", "Ana", "ana-pc", true, false});
    lab.cc.addAgent({"3001", "Carla", "pbx", false, true});
    lab.pbx.addDevice("3001");
    lab.connectPbx();
    SipPhone& ana = lab.phone("ana-pc", "2001", videoPhoneCaps());
    SipPhone& caller = lab.phone("caller", "caller1", videoPhoneCaps());

    AgentDesktop& dAna = lab.desktop("2001");
    AgentDesktop& dCarla = lab.desktop("3001");
    dAna.talkTime = msec(10000);
    dAna.transfers.push_back("3001");
    dCarla.talkTime = msec(4000);
    dAna.start(true);
    dCarla.start(false);
    lab.pump();
    lab.pbx.agentReady("3001", true);  // the desk agent signs in on the PBX
    lab.pump();
    ASSERT_EQ(lab.cc.agentState("3001"), AgentState::Ready);

    std::string id = caller.call("sip:support@cc", "cc", true);
    lab.runUntil(msec(9000));

    // answered by Ana through CSTA AnswerCall, with video, media flowing both ways via the relay
    ASSERT_EQ(ana.callIds().size(), 1u);
    const auto* agentSide = ana.find(ana.callIds()[0]);
    EXPECT_TRUE(ana.videoActive(ana.callIds()[0]));
    StreamReport video = agentSide->media->video().report();
    StreamReport audio = agentSide->media->audio().report();
    EXPECT_EQ(video.codec, "H263");
    EXPECT_EQ(audio.codec, "PCMU");
    EXPECT_GT(video.packetsReceived, 100u);
    EXPECT_GT(audio.packetsReceived, 300u);
    EXPECT_GT(audio.cumulativeLost, 0) << "2% loss on the caller's access link";
    EXPECT_GT(audio.rttMs, 40.0);
    EXPECT_LT(audio.rttMs, 150.0);
    ASSERT_TRUE(agentSide->media->lipSyncOffsetMs().has_value());
    EXPECT_GT(*agentSide->media->lipSyncOffsetMs(), 15.0) << "video path is slower";
    EXPECT_GT(lab.cc.relayedPackets(), 1000u);

    // the transfer to the PBX agent: audio only, answered through the PBX's CSTA interface
    lab.runUntil(msec(14000));
    EXPECT_TRUE(ana.callIds().empty());
    SipPhone* desk = lab.pbx.device("3001");
    ASSERT_EQ(desk->callIds().size(), 1u);
    EXPECT_FALSE(caller.videoActive(id));
    EXPECT_TRUE(caller.find(id)->media->audio().running());

    lab.runUntil(msec(20000));
    EXPECT_EQ(caller.find(id), nullptr) << "Carla's desktop cleared the call";
    const auto& rec = lab.cc.records().at(0);
    EXPECT_EQ(rec.agents, (std::vector<std::string>{"2001", "3001"}));
    EXPECT_EQ(rec.transfers, 1);
    EXPECT_TRUE(rec.videoAtFirstAnswer);
    EXPECT_EQ(rec.outcome, "completed");
    EXPECT_TRUE(dAna.errors.empty());
    EXPECT_TRUE(dCarla.errors.empty());

    bool sawPbxAnswer = false;
    for (const auto& l : lab.cc.log) sawPbxAnswer |= l.find("forwarded to the PBX as AnswerCall") != std::string::npos;
    EXPECT_TRUE(sawPbxAnswer);
    int delivered = 0, established = 0, cleared = 0;
    for (const auto& e : dAna.events) {
        delivered += e.find("DeliveredEvent") != std::string::npos;
        established += e.find("EstablishedEvent") != std::string::npos;
        cleared += e.find("ConnectionClearedEvent") != std::string::npos;
    }
    EXPECT_EQ(delivered, 1);
    EXPECT_EQ(established, 1);
    EXPECT_EQ(cleared, 1);
}

TEST(EndToEnd, LipSyncCorrectionAlignsAudioWithLateVideo) {
    Lab lab(9, msec(5000));
    lab.net.setProfile("caller", "cc", {msec(10), msec(5), 0, msec(80)});
    lab.cc.addAgent({"2001", "Ana", "ana-pc", true, false});
    SipPhone& ana = lab.phone("ana-pc", "2001", videoPhoneCaps());
    ana.autoAnswer = true;  // answers on ring, no desktop needed
    SipPhone& caller = lab.phone("caller", "caller1", videoPhoneCaps());
    lab.cc.handle(SetAgentState{"2001", "ready"});
    caller.call("sip:support@cc", "cc", true);
    lab.runUntil(msec(5000));
    ASSERT_EQ(ana.callIds().size(), 1u);
    auto* media = ana.find(ana.callIds()[0])->media.get();
    double before = media->lipSyncOffsetMs().value();
    EXPECT_GT(before, 50.0);
    media->correctLipSync();
    lab.runUntil(msec(9000));
    EXPECT_LT(std::fabs(media->lipSyncOffsetMs().value()), 10.0);
    EXPECT_NEAR(media->lipSyncCorrectionMs(), before, 0.5);
}
