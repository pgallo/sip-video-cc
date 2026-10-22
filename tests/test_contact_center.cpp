// CSTA-to-SIP mapping and video-aware routing, calling the CSTA provider directly (no sockets)
// and observing the SIP messages the B2BUA sends.
#include "svcc/lab.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using namespace svcc;

namespace {

class CcTest : public ::testing::Test {
protected:
    CcTest() {
        lab.sip.tracing = true;
        lab.cc.setEventSink([this](const CstaMessage& e) { events.push_back(e); });
        lab.cc.addAgent({"2001", "Ana", "ana-pc", true, false});
        lab.cc.addAgent({"2002", "Ben", "ben-phone", false, false});
        lab.cc.addDirectoryNumber("5550100", "customer");
        ana = &lab.phone("ana-pc", "2001", videoPhoneCaps());
        ben = &lab.phone("ben-phone", "2002", audioPhoneCaps());
        caller = &lab.phone("caller", "caller1", videoPhoneCaps());
        customer = &lab.phone("customer", "5550100", videoPhoneCaps());
        customer->autoAnswer = true;
    }

    void ready(const std::string& ext) { ASSERT_TRUE(std::holds_alternative<SetAgentStateResponse>(lab.cc.handle(SetAgentState{ext, "ready"}))); }

    bool traced(const std::string& needle) const {
        return std::any_of(lab.sip.trace.begin(), lab.sip.trace.end(),
                           [&](const std::string& t) { return t.find(needle) != std::string::npos; });
    }

    template <class T>
    std::vector<T> eventsOf() const {
        std::vector<T> out;
        for (const auto& e : events)
            if (auto* t = std::get_if<T>(&e)) out.push_back(*t);
        return out;
    }

    void run(Micros t) { lab.sched.runUntil(lab.sched.now() + t); }

    Lab lab{11, msec(5000)};
    std::vector<CstaMessage> events;
    SipPhone* ana = nullptr;
    SipPhone* ben = nullptr;
    SipPhone* caller = nullptr;
    SipPhone* customer = nullptr;
};

}  // namespace

TEST_F(CcTest, VideoCallerGoesToVideoAgentAndAnswerCallMapsToNotifyTalk) {
    ready("2002");  // idle longer, but has no video
    run(msec(100));
    ready("2001");
    std::string id = caller->call("sip:support@cc", "cc", true);
    run(msec(100));
    ASSERT_TRUE(traced("cc -> ana-pc: INVITE"));
    EXPECT_FALSE(traced("cc -> ben-phone: INVITE"));
    auto delivered = eventsOf<DeliveredEvent>();
    ASSERT_EQ(delivered.size(), 1u);
    EXPECT_TRUE(delivered[0].video);
    EXPECT_EQ(delivered[0].alertingDevice, "2001");

    auto r = lab.cc.handle(AnswerCall{delivered[0].connection});
    EXPECT_TRUE(std::holds_alternative<AnswerCallResponse>(r));
    EXPECT_TRUE(traced("cc -> ana-pc: NOTIFY"));
    run(msec(100));
    ASSERT_EQ(eventsOf<EstablishedEvent>().size(), 1u);
    EXPECT_TRUE(eventsOf<EstablishedEvent>()[0].video);
    EXPECT_TRUE(caller->videoActive(id));
    EXPECT_EQ(lab.cc.agentState("2001"), AgentState::Busy);

    auto again = lab.cc.handle(AnswerCall{delivered[0].connection});
    EXPECT_EQ(std::get<CstaErrorCode>(again).operation, "invalidObjectState");
}

TEST_F(CcTest, VideoCallerFallsBackToAudioAgentAndLosesVideoByReinvite) {
    lab.cc.handle(SetAgentState{"2001", "loggedOn"});  // a video agent is online but not ready
    ready("2002");
    std::string id = caller->call("sip:support@cc", "cc", true);
    run(msec(4000));
    EXPECT_FALSE(traced("cc -> ben-phone: INVITE")) << "waits for a video agent first";
    EXPECT_TRUE(caller->videoActive(id)) << "queue announcement includes video";
    run(msec(1100));
    ASSERT_TRUE(traced("cc -> ben-phone: INVITE"));
    lab.cc.handle(AnswerCall{eventsOf<DeliveredEvent>().at(0).connection});
    run(msec(200));
    EXPECT_FALSE(caller->videoActive(id));
    EXPECT_EQ(lab.cc.records().at(0).videoReinvites, 1);
    EXPECT_FALSE(eventsOf<EstablishedEvent>().at(0).video);
}

TEST_F(CcTest, NoVideoAgentOnlineMeansNoWait) {
    ready("2002");  // Ana is logged out
    caller->call("sip:support@cc", "cc", true);
    run(msec(100));
    EXPECT_TRUE(traced("cc -> ben-phone: INVITE"));
}

TEST_F(CcTest, AudioCallerPrefersAudioAgent) {
    ready("2001");
    run(msec(100));
    ready("2002");
    SipPhone& audioCaller = lab.phone("caller-a", "caller2", audioPhoneCaps());
    audioCaller.call("sip:support@cc", "cc", false);
    run(msec(100));
    EXPECT_TRUE(traced("cc -> ben-phone: INVITE"));
    EXPECT_FALSE(traced("cc -> ana-pc: INVITE"));
}

TEST_F(CcTest, SingleStepTransferMapsToByeAndInviteAndRestoresVideo) {
    lab.cc.handle(SetAgentState{"2001", "loggedOn"});
    ready("2002");
    std::string id = caller->call("sip:support@cc", "cc", true);
    run(msec(5200));  // falls back to Ben (audio)
    lab.cc.handle(AnswerCall{eventsOf<DeliveredEvent>().at(0).connection});
    run(msec(200));
    ASSERT_FALSE(caller->videoActive(id));

    ready("2001");
    auto busy = lab.cc.handle(SingleStepTransferCall{{"C1", "2002"}, "2002"});
    EXPECT_EQ(std::get<CstaErrorCode>(busy).operation, "invalidObjectState");
    auto r = lab.cc.handle(SingleStepTransferCall{{"C1", "2002"}, "2001"});
    ASSERT_TRUE(std::holds_alternative<SingleStepTransferCallResponse>(r));
    EXPECT_EQ(std::get<SingleStepTransferCallResponse>(r).transferredCall.deviceId, "2001");
    run(msec(50));
    EXPECT_TRUE(traced("cc -> ben-phone: BYE"));
    EXPECT_TRUE(traced("cc -> ana-pc: INVITE"));
    auto cleared = eventsOf<ConnectionClearedEvent>();
    ASSERT_EQ(cleared.size(), 1u);
    EXPECT_EQ(cleared[0].cause, "transferred");
    lab.cc.handle(AnswerCall{{"C1", "2001"}});
    run(msec(200));
    EXPECT_TRUE(caller->videoActive(id)) << "re-INVITE adds video back for the video agent";
    EXPECT_EQ(lab.cc.records().at(0).transfers, 1);
    EXPECT_EQ(lab.cc.agentState("2002"), AgentState::Ready);
}

TEST_F(CcTest, ClearConnectionByesBothLegs) {
    ready("2001");
    std::string id = caller->call("sip:support@cc", "cc", true);
    run(msec(100));
    lab.cc.handle(AnswerCall{{"C1", "2001"}});
    run(msec(100));
    auto r = lab.cc.handle(ClearConnection{{"C1", "2001"}});
    EXPECT_TRUE(std::holds_alternative<ClearConnectionResponse>(r));
    EXPECT_TRUE(traced("cc -> ana-pc: BYE"));
    EXPECT_TRUE(traced("cc -> caller: BYE"));
    run(msec(100));
    EXPECT_EQ(caller->find(id), nullptr);
    EXPECT_TRUE(ana->callIds().empty());
    EXPECT_EQ(lab.cc.activeCalls(), 0u);
    EXPECT_EQ(lab.cc.records().at(0).outcome, "completed");
    EXPECT_EQ(lab.cc.agentState("2001"), AgentState::Ready);
}

TEST_F(CcTest, CallerHangsUpWhileAgentRingsSoTheB2buaCancels) {
    ready("2001");
    std::string id = caller->call("sip:support@cc", "cc", true);
    run(msec(100));
    caller->hangup(id);
    run(msec(100));
    EXPECT_TRUE(traced("cc -> ana-pc: CANCEL"));
    EXPECT_TRUE(traced("ana-pc -> cc: 487"));
    EXPECT_TRUE(ana->callIds().empty());
    EXPECT_EQ(lab.cc.records().at(0).outcome, "abandoned");
}

TEST_F(CcTest, MakeCallIsThirdPartyCallControl) {
    ready("2001");
    auto r = lab.cc.handle(MakeCall{"2001", "5550100"});
    ASSERT_TRUE(std::holds_alternative<MakeCallResponse>(r));
    std::string id = std::get<MakeCallResponse>(r).callingDevice.callId;
    run(msec(100));
    EXPECT_TRUE(traced("cc -> ana-pc: INVITE"));
    EXPECT_TRUE(traced("cc -> customer: INVITE"));
    auto est = eventsOf<EstablishedEvent>();
    ASSERT_EQ(est.size(), 1u);
    EXPECT_EQ(est[0].cause, "makeCall");
    ASSERT_EQ(customer->callIds().size(), 1u);
    EXPECT_TRUE(customer->videoActive(customer->callIds()[0]));
    lab.cc.handle(ClearConnection{{id, "2001"}});
    run(msec(100));
    EXPECT_TRUE(customer->callIds().empty());
}

TEST_F(CcTest, ErrorsForUnknownObjects) {
    EXPECT_EQ(std::get<CstaErrorCode>(lab.cc.handle(AnswerCall{{"C9", "2001"}})).operation, "invalidCallID");
    EXPECT_EQ(std::get<CstaErrorCode>(lab.cc.handle(SetAgentState{"7777", "ready"})).operation, "invalidDeviceID");
    EXPECT_EQ(std::get<CstaErrorCode>(lab.cc.handle(MakeCall{"2001", "5550100"})).operation, "invalidObjectState")
        << "agent not ready";
    ready("2001");
    EXPECT_EQ(std::get<CstaErrorCode>(lab.cc.handle(MakeCall{"2001", "000"})).operation, "invalidCalledDeviceID");
}

TEST_F(CcTest, AgentHoldIsPropagatedToTheCaller) {
    ready("2001");
    std::string id = caller->call("sip:support@cc", "cc", true);
    run(msec(100));
    lab.cc.handle(AnswerCall{{"C1", "2001"}});
    run(msec(100));
    std::string agentCall = ana->callIds().at(0);
    ana->reinvite(agentCall, true, true);
    run(msec(100));
    for (const auto& s : caller->find(id)->streams)
        if (s.active) EXPECT_EQ(s.localDir, Direction::RecvOnly) << s.type;
    ana->reinvite(agentCall, true, false);
    run(msec(100));
    for (const auto& s : caller->find(id)->streams)
        if (s.active) EXPECT_EQ(s.localDir, Direction::SendRecv) << s.type;
}
