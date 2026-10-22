// The video contact center: a SIP back-to-back user agent that anchors media, a video-aware
// queue and router, and a CSTA provider. CSTA is the integration point for third-party call
// control: agent desktops and CTI applications drive calls through the CSTA server in front of
// this class, and agents whose phones live on a commercial PBX are reached over a SIP trunk and
// controlled through that PBX's own CSTA interface (attachPbx).
#pragma once

#include "svcc/csta.hpp"
#include "svcc/csta_net.hpp"
#include "svcc/media.hpp"
#include "svcc/sdp.hpp"
#include "svcc/sip.hpp"

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace svcc {

struct AgentConfig {
    std::string ext;
    std::string name;
    std::string sipAddress;  // network address of the agent's phone, or of the PBX trunk
    bool video = false;
    bool onPbx = false;
};

enum class AgentState { LoggedOut, NotReady, Ready, Busy };
const char* toString(AgentState s);

struct CallRecord {
    std::string callId;
    std::string caller;
    bool callerOfferedVideo = false;
    bool outbound = false;
    std::vector<std::string> agents;
    Micros arrived = 0;
    Micros answered = -1;
    Micros ended = -1;
    int transfers = 0;
    int videoReinvites = 0;
    bool videoAtFirstAnswer = false;
    std::string outcome;
};

class ContactCenter : public SipEndpoint, public CstaProvider {
public:
    ContactCenter(Scheduler& sched, SipNetwork& sip, MediaNetwork& net, std::string address, Micros videoFallbackWait);
    ~ContactCenter() override;

    void addAgent(const AgentConfig& a);
    void addDirectoryNumber(const std::string& number, const std::string& sipAddress);
    void setEventSink(std::function<void(const CstaMessage&)> sink) { sink_ = std::move(sink); }

    // Commercial PBX integration: monitor the PBX-resident agent devices and use the link for
    // third-party call control (AnswerCall, ClearConnection) of calls delivered over the trunk.
    void attachPbx(CstaClient* link);
    void onPbxEvent(const CstaMessage& event);

    void onSip(const SipMessage& msg, const std::string& from) override;
    bool hasDevice(const std::string& device) const override;
    CstaMessage handle(const CstaMessage& request) override;

    AgentState agentState(const std::string& ext) const;
    const std::vector<CallRecord>& records() const { return records_; }
    std::size_t activeCalls() const { return calls_.size(); }
    std::size_t queued() const { return queue_.size(); }
    std::uint64_t relayedPackets() const;
    std::vector<std::string> log;

private:
    struct Leg;
    struct CcCall;
    struct Agent {
        AgentConfig cfg;
        AgentState state = AgentState::LoggedOut;
        Micros idleSince = 0;
    };

    void inboundInvite(const SipMessage& m, const std::string& from);
    void onLegRequest(CcCall& call, Leg& leg, bool callerSide, const SipMessage& m);
    void onLegResponse(CcCall& call, Leg& leg, bool callerSide, const SipMessage& m);
    void route();
    void offer(CcCall& call, Agent& agent);
    void agentAnswered(CcCall& call);
    void alignVideoAndBridge(CcCall& call);
    void bridge(CcCall& call);
    void reoffer(CcCall& call, Leg& leg, bool video, Direction dir, std::function<void()> then);
    void dropAgentLeg(CcCall& call, const std::string& cause, bool sendBye);
    void endCall(CcCall& call, const std::string& outcome, bool callerAlreadyGone, bool agentAlreadyGone);
    void setReady(Agent& a);
    void emit(CstaMessage event);
    void note(const std::string& line);
    std::unique_ptr<Leg> newLeg(bool uac);
    void sendOnLeg(Leg& leg, const SipMessage& m);
    CallRecord& record(CcCall& call);

    Scheduler& sched_;
    SipNetwork& sip_;
    MediaNetwork& net_;
    std::string address_;
    Micros videoFallbackWait_;
    Capabilities caps_;
    std::function<void(const CstaMessage&)> sink_;
    CstaClient* pbx_ = nullptr;
    std::map<std::string, Agent> agents_;
    std::map<std::string, std::string> directory_;
    std::map<std::string, std::unique_ptr<CcCall>> calls_;
    std::map<std::string, std::pair<std::string, bool>> legIndex_;  // SIP Call-ID -> (call id, caller side)
    std::deque<std::string> queue_;
    std::vector<std::unique_ptr<CcCall>> graveyard_;
    std::vector<CallRecord> records_;
    std::uint64_t relayedFinished_ = 0;
    int nextCall_ = 0;
    int nextPort_ = 20000;
};

}  // namespace svcc
