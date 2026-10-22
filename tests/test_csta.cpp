#include "svcc/csta.hpp"
#include "svcc/csta_net.hpp"
#include "svcc/xml.hpp"

#include <gtest/gtest.h>

using namespace svcc;

TEST(CstaXml, RequestsRoundTrip) {
    auto rt = [](const CstaMessage& m) { return decodeXml(encodeXml(m)); };
    EXPECT_EQ(std::get<MonitorStart>(rt(MonitorStart{"2001"})).device, "2001");
    auto mc = std::get<MakeCall>(rt(MakeCall{"2001", "5550100"}));
    EXPECT_EQ(mc.callingDevice, "2001");
    EXPECT_EQ(mc.calledDirectoryNumber, "5550100");
    EXPECT_EQ(std::get<AnswerCall>(rt(AnswerCall{{"C1", "2001"}})).callToBeAnswered, (ConnectionId{"C1", "2001"}));
    EXPECT_EQ(std::get<ClearConnection>(rt(ClearConnection{{"C1", "2002"}})).connectionToBeCleared.deviceId, "2002");
    auto sst = std::get<SingleStepTransferCall>(rt(SingleStepTransferCall{{"C1", "2001"}, "2002"}));
    EXPECT_EQ(sst.activeCall.callId, "C1");
    EXPECT_EQ(sst.transferredTo, "2002");
    auto sas = std::get<SetAgentState>(rt(SetAgentState{"2001", "notReady"}));
    EXPECT_EQ(sas.requestedAgentState, "notReady");
}

TEST(CstaXml, ResponsesAndEventsRoundTrip) {
    auto rt = [](const CstaMessage& m) { return decodeXml(encodeXml(m)); };
    EXPECT_EQ(std::get<MonitorStartResponse>(rt(MonitorStartResponse{"M7"})).crossRefId, "M7");
    EXPECT_EQ(std::get<MakeCallResponse>(rt(MakeCallResponse{{"C4", "2001"}})).callingDevice.callId, "C4");
    EXPECT_TRUE(std::holds_alternative<AnswerCallResponse>(rt(AnswerCallResponse{})));
    EXPECT_EQ(std::get<CstaErrorCode>(rt(CstaErrorCode{"invalidCallID"})).operation, "invalidCallID");

    DeliveredEvent d;
    d.crossRefId = "M1";
    d.connection = {"C1", "2001"};
    d.alertingDevice = "2001";
    d.callingDevice = "caller<1>&co";  // escaped and restored
    d.calledDevice = "support";
    d.video = true;
    d.correlatorData = "C1";
    auto d2 = std::get<DeliveredEvent>(rt(d));
    EXPECT_EQ(d2.callingDevice, "caller<1>&co");
    EXPECT_TRUE(d2.video);
    EXPECT_EQ(d2.correlatorData, "C1");
    EXPECT_EQ(eventDevice(d2), "2001");

    EstablishedEvent e;
    e.establishedConnection = {"C1", "2002"};
    e.answeringDevice = "2002";
    auto e2 = std::get<EstablishedEvent>(rt(e));
    EXPECT_FALSE(e2.video);
    EXPECT_EQ(eventDevice(e2), "2002");

    ConnectionClearedEvent c;
    c.droppedConnection = {"C1", "2001"};
    c.cause = "transferred";
    EXPECT_EQ(std::get<ConnectionClearedEvent>(rt(c)).cause, "transferred");
    EXPECT_EQ(std::get<AgentReadyEvent>(rt(AgentReadyEvent{"M2", "3001"})).agentDevice, "3001");
    EXPECT_TRUE(isEvent(AgentNotReadyEvent{}));
    EXPECT_FALSE(isEvent(AnswerCall{}));

    std::string xml = encodeXml(d);
    EXPECT_NE(xml.find("xmlns=\"http://www.ecma-international.org/standards/ecma-323/csta/ed3\""), std::string::npos);
    EXPECT_NE(xml.find("<alertingDevice><deviceIdentifier>2001</deviceIdentifier></alertingDevice>"), std::string::npos);
}

TEST(CstaXml, AcceptsPrefixesWhitespaceAndRejectsBadInput) {
    auto m = decodeXml("<?xml version=\"1.0\"?>\n<csta:AnswerCall xmlns:csta=\"urn:x\">\n  <csta:callToBeAnswered>"
                       "<csta:callID>9</csta:callID><csta:deviceID>2001</csta:deviceID></csta:callToBeAnswered>"
                       "<!-- comment --></csta:AnswerCall>");
    EXPECT_EQ(std::get<AnswerCall>(m).callToBeAnswered.callId, "9");
    EXPECT_THROW(decodeXml("<AnswerCall><callToBeAnswered></AnswerCall>"), CstaDecodeError);
    EXPECT_THROW(decodeXml("<AnswerCall/>"), CstaDecodeError);
    EXPECT_THROW(decodeXml("<Unknown/>"), CstaDecodeError);
    EXPECT_THROW(decodeXml("<SetAgentState><device>1</device><requestedAgentState>asleep</requestedAgentState></SetAgentState>"),
                 CstaDecodeError);
    EXPECT_THROW(decodeXml("<MakeCall><callingDevice>1&bogus;</callingDevice></MakeCall>"), CstaDecodeError);
    EXPECT_THROW(decodeXml("not xml"), CstaDecodeError);
}

TEST(CstaFraming, ReassemblesSplitAndCoalescedFrames) {
    std::string wire = frame(12, "<a/>") + frame(kEventInvokeId, "<b>x</b>");
    EXPECT_EQ(wire.substr(4, 4), "0012");
    FrameDecoder dec;
    std::vector<std::pair<int, std::string>> out;
    for (char c : wire) {  // one byte at a time
        dec.feed(&c, 1);
        int id;
        std::string xml;
        while (dec.next(id, xml)) out.emplace_back(id, xml);
    }
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], (std::pair<int, std::string>{12, "<a/>"}));
    EXPECT_EQ(out[1], (std::pair<int, std::string>{9999, "<b>x</b>"}));

    FrameDecoder bad;
    std::string garbage("XX\x00\x10" "0001<a/>", 12);
    bad.feed(garbage.data(), garbage.size());
    int id;
    std::string xml;
    EXPECT_THROW(bad.next(id, xml), CstaDecodeError);
}

namespace {
class FakeProvider : public CstaProvider {
public:
    bool hasDevice(const std::string& d) const override { return d == "2001" || d == "2002"; }
    CstaMessage handle(const CstaMessage& r) override {
        if (std::holds_alternative<AnswerCall>(r)) return AnswerCallResponse{};
        return CstaErrorCode{"requestIncompatibleWithObject"};
    }
};

void pump(CstaServer& s, std::vector<CstaClient*> clients) {
    for (int quiet = 0; quiet < 3;) {
        int n = s.poll(0);
        for (auto* c : clients) n += c->poll(0);
        quiet = n == 0 ? quiet + 1 : 0;
    }
}
}  // namespace

TEST(CstaTcp, MonitorRequestCorrelationAndEventRouting) {
    FakeProvider provider;
    CstaServer server(provider);
    CstaClient a(server.port());
    CstaClient b(server.port());
    std::vector<CstaMessage> aEvents, bEvents, responses;
    a.onEvent = [&](const CstaMessage& m) { aEvents.push_back(m); };
    b.onEvent = [&](const CstaMessage& m) { bEvents.push_back(m); };
    auto keep = [&](const CstaMessage& m) { responses.push_back(m); };

    a.request(MonitorStart{"2001"}, keep);
    b.request(MonitorStart{"2002"}, keep);
    b.request(MonitorStart{"9999"}, keep);
    a.request(AnswerCall{{"C1", "2001"}}, keep);
    a.request(MakeCall{"2001", "1"}, keep);
    pump(server, {&a, &b});
    ASSERT_EQ(responses.size(), 5u);
    EXPECT_EQ(server.sessions(), 2u);
    int monitors = 0, errors = 0, answers = 0;
    for (const auto& r : responses) {
        monitors += std::holds_alternative<MonitorStartResponse>(r);
        answers += std::holds_alternative<AnswerCallResponse>(r);
        if (auto* e = std::get_if<CstaErrorCode>(&r)) {
            ++errors;
            EXPECT_TRUE(e->operation == "invalidDeviceID" || e->operation == "requestIncompatibleWithObject");
        }
    }
    EXPECT_EQ(monitors, 2);
    EXPECT_EQ(answers, 1);
    EXPECT_EQ(errors, 2);
    EXPECT_EQ(a.pending(), 0u);

    DeliveredEvent d;
    d.connection = {"C1", "2001"};
    d.alertingDevice = "2001";
    server.publish(d);
    server.publish(AgentReadyEvent{"", "2002"});
    server.publish(AgentReadyEvent{"", "3001"});  // nobody monitors it
    pump(server, {&a, &b});
    ASSERT_EQ(aEvents.size(), 1u);
    ASSERT_EQ(bEvents.size(), 1u);
    EXPECT_EQ(std::get<DeliveredEvent>(aEvents[0]).crossRefId, "M1");
    EXPECT_EQ(std::get<AgentReadyEvent>(bEvents[0]).crossRefId, "M2");
}
