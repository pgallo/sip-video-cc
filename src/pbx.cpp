#include "svcc/pbx.hpp"

namespace svcc {

SimulatedPbx::SimulatedPbx(Scheduler& sched, SipNetwork& sip, MediaNetwork& net, std::string address)
    : sched_(sched), sip_(sip), net_(net), address_(std::move(address)) {}

void SimulatedPbx::addDevice(const std::string& ext) {
    Capabilities caps;
    caps.audio = {codecs::PCMA};
    auto phone = std::make_unique<SipPhone>(sched_, sip_, net_, address_, ext, caps, nextPort_);
    nextPort_ += 400;
    SipPhone* p = phone.get();
    p->onIncoming = [this, ext](const std::string& sipCallId, const SipMessage& invite) {
        std::string id = "P" + std::to_string(++nextCall_);
        std::string from = invite.header("From");
        calls_[id] = {ext, sipCallId, uriUser(from.substr(from.find('<') + 1))};
        DeliveredEvent e;
        e.connection = {id, ext};
        e.alertingDevice = ext;
        e.callingDevice = calls_[id].caller;
        e.calledDevice = ext;
        e.correlatorData = invite.header("User-to-User");
        if (sink_) sink_(e);
    };
    p->onEstablished = [this, ext](const std::string& sipCallId) {
        std::string id = pbxCallFor(sipCallId);
        EstablishedEvent e;
        e.establishedConnection = {id, ext};
        e.answeringDevice = ext;
        e.callingDevice = calls_.count(id) ? calls_[id].caller : "";
        e.calledDevice = ext;
        if (sink_) sink_(e);
    };
    p->onEnded = [this, ext](const std::string& sipCallId) {
        std::string id = pbxCallFor(sipCallId);
        ConnectionClearedEvent e;
        e.droppedConnection = {id, ext};
        e.releasingDevice = ext;
        if (sink_) sink_(e);
        calls_.erase(id);
    };
    devices_[ext] = std::move(phone);
}

std::string SimulatedPbx::pbxCallFor(const std::string& sipCallId) const {
    for (const auto& kv : calls_)
        if (kv.second.sipCallId == sipCallId) return kv.first;
    return "";
}

SipPhone* SimulatedPbx::device(const std::string& ext) {
    auto it = devices_.find(ext);
    return it == devices_.end() ? nullptr : it->second.get();
}

void SimulatedPbx::onSip(const SipMessage& msg, const std::string& from) {
    std::string ext;
    auto known = sipCallToExt_.find(msg.callId());
    if (known != sipCallToExt_.end()) {
        ext = known->second;
    } else if (msg.isRequest && msg.method == "INVITE") {
        ext = uriUser(msg.uri);
        if (!devices_.count(ext)) {
            sip_.send(address_, from, makeResponse(msg, 404, "Not Found", "pbx404"));
            return;
        }
        sipCallToExt_[msg.callId()] = ext;
    } else {
        return;
    }
    devices_[ext]->onSip(msg, from);
}

CstaMessage SimulatedPbx::handle(const CstaMessage& request) {
    if (auto* r = std::get_if<AnswerCall>(&request)) {
        auto it = calls_.find(r->callToBeAnswered.callId);
        if (it == calls_.end() || it->second.ext != r->callToBeAnswered.deviceId) return CstaErrorCode{"invalidCallID"};
        devices_[it->second.ext]->answer(it->second.sipCallId);
        return AnswerCallResponse{};
    }
    if (auto* r = std::get_if<ClearConnection>(&request)) {
        auto it = calls_.find(r->connectionToBeCleared.callId);
        if (it == calls_.end()) return CstaErrorCode{"invalidCallID"};
        devices_[it->second.ext]->hangup(it->second.sipCallId);
        return ClearConnectionResponse{};
    }
    if (auto* r = std::get_if<SetAgentState>(&request)) {
        if (!hasDevice(r->device)) return CstaErrorCode{"invalidDeviceID"};
        agentReady(r->device, r->requestedAgentState == "ready");
        return SetAgentStateResponse{};
    }
    return CstaErrorCode{"serviceNotSupported"};
}

void SimulatedPbx::agentReady(const std::string& ext, bool ready) {
    if (!sink_) return;
    if (ready)
        sink_(AgentReadyEvent{"", ext});
    else
        sink_(AgentNotReadyEvent{"", ext});
}

void SimulatedPbx::hangup(const std::string& ext) {
    if (SipPhone* p = device(ext)) p->hangupAll();
}

}  // namespace svcc
