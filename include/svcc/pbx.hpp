// A simulated commercial PBX for tests and the demo: audio-only desk phones reached over a SIP
// trunk, controlled and observed through the PBX's own CSTA interface (third-party call control).
// Calls offered over the trunk ring until a CSTA AnswerCall arrives; every state change is
// reported as a CSTA event carrying the trunk call's user-to-user data as correlator.
#pragma once

#include "svcc/csta_net.hpp"
#include "svcc/phone.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace svcc {

class SimulatedPbx : public SipEndpoint, public CstaProvider {
public:
    SimulatedPbx(Scheduler& sched, SipNetwork& sip, MediaNetwork& net, std::string address = "pbx");

    void addDevice(const std::string& ext);
    void setEventSink(std::function<void(const CstaMessage&)> sink) { sink_ = std::move(sink); }

    void onSip(const SipMessage& msg, const std::string& from) override;
    bool hasDevice(const std::string& device) const override { return devices_.count(device) > 0; }
    CstaMessage handle(const CstaMessage& request) override;

    // simulation API: what a person does at the desk phone
    void agentReady(const std::string& ext, bool ready);
    void hangup(const std::string& ext);
    SipPhone* device(const std::string& ext);

private:
    struct PbxCall {
        std::string ext;
        std::string sipCallId;
        std::string caller;
    };
    std::string pbxCallFor(const std::string& sipCallId) const;

    Scheduler& sched_;
    SipNetwork& sip_;
    MediaNetwork& net_;
    std::string address_;
    std::function<void(const CstaMessage&)> sink_;
    std::map<std::string, std::unique_ptr<SipPhone>> devices_;
    std::map<std::string, std::string> sipCallToExt_;
    std::map<std::string, PbxCall> calls_;  // PBX call id -> call
    int nextCall_ = 0;
    int nextPort_ = 40000;
};

}  // namespace svcc
