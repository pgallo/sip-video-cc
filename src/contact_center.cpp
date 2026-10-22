#include "svcc/contact_center.hpp"

#include <cstdio>

namespace svcc {

const char* toString(AgentState s) {
    switch (s) {
        case AgentState::LoggedOut: return "LoggedOut";
        case AgentState::NotReady: return "NotReady";
        case AgentState::Ready: return "Ready";
        case AgentState::Busy: return "Busy";
    }
    return "?";
}

struct ContactCenter::Leg {
    Dialog dialog;
    bool uac = false;
    bool ringing = false;
    bool established = false;
    bool awaitingAck = false;
    bool delivered = false;
    MediaPorts ports{0, 0};
    SessionDescription local;
    SessionDescription remote;
    std::vector<NegotiatedStream> streams;
    bool reofferPending = false;
    SessionDescription pendingOffer;
    std::function<void()> afterReoffer;

    const NegotiatedStream* stream(const std::string& type) const {
        for (const auto& s : streams)
            if (s.type == type && s.active) return &s;
        return nullptr;
    }
    bool video() const { return stream("video") != nullptr; }
};

struct ContactCenter::CcCall {
    std::string id;
    std::string callerUser;
    std::string dialed;
    bool outbound = false;
    std::string outboundNumber;
    std::unique_ptr<Leg> caller;
    std::unique_ptr<Leg> agent;
    std::string agentExt;
    bool callerCanVideo = false;
    Micros queuedAt = 0;
    std::unique_ptr<MediaSession> announcer;
    std::unique_ptr<MediaRelay> relay;
    std::string pbxCallId;
    std::size_t recordIndex = 0;
};

namespace {

std::string stripAngles(const std::string& v) {
    auto lt = v.find('<');
    auto gt = v.find('>');
    if (lt == std::string::npos || gt == std::string::npos) return v.substr(0, v.find(';'));
    return v.substr(lt + 1, gt - lt - 1);
}

CstaMessage error(const std::string& op) { return CstaErrorCode{op}; }

}  // namespace

ContactCenter::ContactCenter(Scheduler& sched, SipNetwork& sip, MediaNetwork& net, std::string address,
                             Micros videoFallbackWait)
    : sched_(sched), sip_(sip), net_(net), address_(std::move(address)), videoFallbackWait_(videoFallbackWait) {
    caps_.audio = {codecs::PCMU, codecs::PCMA};
    caps_.video = {codecs::H263, codecs::H261};
}

ContactCenter::~ContactCenter() = default;

CallRecord& ContactCenter::record(CcCall& call) { return records_[call.recordIndex]; }

void ContactCenter::addAgent(const AgentConfig& a) { agents_[a.ext] = Agent{a}; }

void ContactCenter::addDirectoryNumber(const std::string& number, const std::string& sipAddress) {
    directory_[number] = sipAddress;
}

void ContactCenter::note(const std::string& line) {
    char t[32];
    std::snprintf(t, sizeof t, "[%8.3fs] ", static_cast<double>(sched_.now()) / 1e6);
    log.push_back(t + line);
}

void ContactCenter::emit(CstaMessage event) {
    if (sink_) sink_(event);
}

std::unique_ptr<ContactCenter::Leg> ContactCenter::newLeg(bool uac) {
    auto leg = std::make_unique<Leg>();
    leg->uac = uac;
    leg->ports = {nextPort_, nextPort_ + 2};
    nextPort_ += 4;
    return leg;
}

void ContactCenter::sendOnLeg(Leg& leg, const SipMessage& m) { sip_.send(address_, leg.dialog.remoteAddr, m); }

std::uint64_t ContactCenter::relayedPackets() const {
    std::uint64_t n = relayedFinished_;
    for (const auto& kv : calls_)
        if (kv.second->relay) n += kv.second->relay->forwarded();
    return n;
}

AgentState ContactCenter::agentState(const std::string& ext) const {
    auto it = agents_.find(ext);
    return it == agents_.end() ? AgentState::LoggedOut : it->second.state;
}

bool ContactCenter::hasDevice(const std::string& device) const { return agents_.count(device) > 0; }

// ------------------------------------------------------------------ SIP side

void ContactCenter::onSip(const SipMessage& m, const std::string& from) {
    graveyard_.clear();
    auto idx = legIndex_.find(m.callId());
    if (idx == legIndex_.end()) {
        if (m.isRequest && m.method == "INVITE" && m.toTag().empty())
            inboundInvite(m, from);
        else if (m.isRequest && m.method != "ACK")
            sip_.send(address_, from, makeResponse(m, 481, "Call/Transaction Does Not Exist"));
        return;
    }
    auto c = calls_.find(idx->second.first);
    if (c == calls_.end()) return;
    CcCall& call = *c->second;
    bool callerSide = idx->second.second;
    Leg* leg = callerSide ? call.caller.get() : call.agent.get();
    if (!leg || leg->dialog.callId != m.callId()) return;
    if (m.isRequest)
        onLegRequest(call, *leg, callerSide, m);
    else
        onLegResponse(call, *leg, callerSide, m);
}

void ContactCenter::inboundInvite(const SipMessage& m, const std::string& from) {
    SessionDescription offer;
    try {
        offer = SessionDescription::parse(m.body);
    } catch (const SdpError&) {
        sip_.send(address_, from, makeResponse(m, 400, "Bad Request"));
        return;
    }
    auto call = std::make_unique<CcCall>();
    call->id = "C" + std::to_string(++nextCall_);
    call->callerUser = uriUser(stripAngles(m.header("From")));
    call->dialed = uriUser(m.uri);
    auto leg = newLeg(false);
    leg->dialog.callId = m.callId();
    leg->dialog.localTag = "cc" + call->id;
    leg->dialog.remoteTag = m.fromTag();
    leg->dialog.localUri = stripAngles(m.header("To"));
    leg->dialog.remoteUri = stripAngles(m.header("From"));
    leg->dialog.remoteAddr = from;
    leg->dialog.localSeq = 100;
    leg->remote = offer;
    leg->local = makeAnswer(offer, caps_, address_, leg->ports);
    leg->local.user = "cc";
    if (leg->local.media.empty() || leg->local.media.front().port == 0) {
        sip_.send(address_, from, makeResponse(m, 488, "Not Acceptable Here", leg->dialog.localTag));
        return;
    }
    leg->awaitingAck = true;
    call->callerCanVideo = offer.hasActive("video");
    call->recordIndex = records_.size();
    CallRecord rec;
    rec.callId = call->id;
    rec.caller = call->callerUser;
    rec.callerOfferedVideo = call->callerCanVideo;
    rec.arrived = sched_.now();
    records_.push_back(rec);

    SipMessage ok = makeResponse(m, 200, "OK", leg->dialog.localTag);
    ok.add("Contact", "<sip:" + call->dialed + "@" + address_ + ">");
    ok.add("Content-Type", "application/sdp");
    ok.body = leg->local.toString();
    sendOnLeg(*leg, ok);
    note(call->id + " INVITE from " + call->callerUser + " offering " +
         (call->callerCanVideo ? "audio+video" : "audio only") + "; B2BUA answers leg A");
    legIndex_[m.callId()] = {call->id, true};
    call->caller = std::move(leg);
    calls_[call->id] = std::move(call);
}

void ContactCenter::onLegRequest(CcCall& call, Leg& leg, bool callerSide, const SipMessage& m) {
    if (m.method == "ACK") {
        if (!leg.awaitingAck) return;
        leg.awaitingAck = false;
        leg.streams = negotiate(leg.local, leg.remote, leg.local);
        if (!leg.established) {  // the caller's initial INVITE: start queueing
            leg.established = true;
            call.announcer = std::make_unique<MediaSession>(sched_, net_, address_, leg.ports,
                                                            0xCC000000u + static_cast<std::uint32_t>(nextCall_) * 16);
            call.announcer->apply(leg.streams);
            call.queuedAt = sched_.now();
            queue_.push_back(call.id);
            note(call.id + " queued; " + (leg.video() ? "video announcement and audio prompt" : "audio prompt") +
                 " streaming to caller");
            if (leg.video()) sched_.schedule(videoFallbackWait_ + 1, [this]() { route(); });
            route();
            return;
        }
        // ACK for a re-INVITE we answered: propagate the change to the other leg, then re-bridge
        Leg* other = callerSide ? call.agent.get() : call.caller.get();
        if (other && other->established) {
            bool otherCanVideo = callerSide ? agents_[call.agentExt].cfg.video : call.callerCanVideo;
            const MediaDescription* audio = leg.remote.find("audio");
            Direction dir = audio ? audio->dir : Direction::SendRecv;
            std::string id = call.id;
            note(call.id + " re-INVITE from " + (callerSide ? call.callerUser : call.agentExt) + " (" + toString(dir) +
                 (leg.video() ? ", video" : ", no video") + ") propagated to the other leg");
            reoffer(call, *other, leg.video() && otherCanVideo, dir, [this, id]() {
                auto it = calls_.find(id);
                if (it != calls_.end()) bridge(*it->second);
            });
        } else {
            bridge(call);
        }
        return;
    }
    if (m.method == "INVITE") {  // re-INVITE on an established leg
        try {
            leg.remote = SessionDescription::parse(m.body);
        } catch (const SdpError&) {
            sendOnLeg(leg, makeResponse(m, 400, "Bad Request"));
            return;
        }
        leg.local = makeAnswer(leg.remote, caps_, address_, leg.ports);
        leg.local.user = "cc";
        leg.awaitingAck = true;
        SipMessage ok = makeResponse(m, 200, "OK");
        ok.add("Content-Type", "application/sdp");
        ok.body = leg.local.toString();
        sendOnLeg(leg, ok);
        return;
    }
    if (m.method == "BYE") {
        sendOnLeg(leg, makeResponse(m, 200, "OK"));
        if (callerSide)
            endCall(call, call.agent && call.agent->established ? "completed" : "abandoned", true, false);
        else
            endCall(call, "completed", false, true);
        return;
    }
    sendOnLeg(leg, makeResponse(m, 200, "OK"));
}

void ContactCenter::onLegResponse(CcCall& call, Leg& leg, bool callerSide, const SipMessage& m) {
    if (m.cseqMethod() != "INVITE") return;
    if (m.status < 200) {
        if (!m.toTag().empty()) leg.dialog.remoteTag = m.toTag();
        if (m.status == 180 && !callerSide && !leg.delivered && !call.outbound) {
            leg.ringing = true;
            leg.delivered = true;
            DeliveredEvent e;
            e.connection = {call.id, call.agentExt};
            e.alertingDevice = call.agentExt;
            e.callingDevice = call.callerUser;
            e.calledDevice = call.outbound ? call.outboundNumber : call.dialed;
            e.video = leg.local.hasActive("video");
            emit(e);
        }
        return;
    }
    if (m.status >= 300) {
        if (leg.reofferPending) {
            leg.reofferPending = false;
            auto then = std::move(leg.afterReoffer);
            if (then) then();
        } else if (!callerSide) {
            std::string ext = call.agentExt;
            note(call.id + " agent " + ext + " rejected the call (" + std::to_string(m.status) + "), requeued");
            legIndex_.erase(leg.dialog.callId);
            call.agent.reset();
            call.agentExt.clear();
            agents_[ext].state = AgentState::NotReady;
            emit(AgentNotReadyEvent{"", ext});
            queue_.push_front(call.id);
            route();
        } else {
            endCall(call, "failed", true, false);
        }
        return;
    }
    leg.dialog.remoteTag = m.toTag();
    SipMessage ack;
    ack.method = "ACK";
    ack.uri = leg.dialog.remoteUri;
    ack.add("From", "<" + leg.dialog.localUri + ">;tag=" + leg.dialog.localTag);
    ack.add("To", "<" + leg.dialog.remoteUri + ">;tag=" + leg.dialog.remoteTag);
    ack.add("Call-ID", leg.dialog.callId);
    ack.add("CSeq", std::to_string(m.cseq()) + " ACK");
    sendOnLeg(leg, ack);
    if (leg.reofferPending) {
        leg.reofferPending = false;
        leg.local = leg.pendingOffer;
        leg.remote = SessionDescription::parse(m.body);
        leg.streams = negotiate(leg.local, leg.remote, leg.remote);
        auto then = std::move(leg.afterReoffer);
        if (then) then();
        return;
    }
    if (leg.established) return;
    leg.remote = SessionDescription::parse(m.body);
    leg.streams = negotiate(leg.local, leg.remote, leg.remote);
    leg.established = true;
    leg.ringing = false;
    if (callerSide) {  // outbound: the far end answered
        record(call).answered = sched_.now();
        note(call.id + " destination " + call.outboundNumber + " answered");
        bridge(call);
        return;
    }
    if (call.outbound && !call.caller) {  // MakeCall: the agent's phone auto-answered, now call out
        agents_[call.agentExt].state = AgentState::Busy;
        EstablishedEvent e;
        e.establishedConnection = {call.id, call.agentExt};
        e.answeringDevice = call.agentExt;
        e.callingDevice = call.agentExt;
        e.calledDevice = call.outboundNumber;
        e.video = leg.video();
        e.cause = "makeCall";
        emit(e);
        record(call).agents.push_back(call.agentExt);
        record(call).videoAtFirstAnswer = leg.video();
        auto out = newLeg(true);
        out->dialog.callId = call.id + "-out@" + address_;
        out->dialog.localTag = "cc" + call.id + "o";
        out->dialog.localUri = "sip:" + call.agentExt + "@" + address_;
        std::string dest = directory_[call.outboundNumber];
        out->dialog.remoteUri = "sip:" + call.outboundNumber + "@" + dest;
        out->dialog.remoteAddr = dest;
        out->dialog.localSeq = 0;
        out->local = makeOffer(caps_, address_, out->ports, leg.video());
        out->local.user = "cc";
        SipMessage inv = out->dialog.request("INVITE");
        inv.add("Contact", "<sip:" + call.agentExt + "@" + address_ + ">");
        inv.add("Content-Type", "application/sdp");
        inv.body = out->local.toString();
        sendOnLeg(*out, inv);
        legIndex_[out->dialog.callId] = {call.id, true};
        note(call.id + " MakeCall: agent " + call.agentExt + " connected, INVITE to " + call.outboundNumber);
        call.caller = std::move(out);
        return;
    }
    agentAnswered(call);
}

// ------------------------------------------------------------------ routing and bridging

void ContactCenter::route() {
    for (auto it = queue_.begin(); it != queue_.end();) {
        auto c = calls_.find(*it);
        if (c == calls_.end()) {
            it = queue_.erase(it);
            continue;
        }
        CcCall& call = *c->second;
        bool videoCaller = call.caller->video();
        auto pick = [this](auto pred) {
            Agent* best = nullptr;
            for (auto& kv : agents_) {
                Agent& a = kv.second;
                if (a.state == AgentState::Ready && pred(a) && (!best || a.idleSince < best->idleSince)) best = &a;
            }
            return best;
        };
        Agent* chosen = nullptr;
        if (videoCaller) {
            chosen = pick([](const Agent& a) { return a.cfg.video; });
            if (!chosen) {
                bool videoAgentOnline = false;
                for (const auto& kv : agents_)
                    videoAgentOnline |= kv.second.cfg.video && kv.second.state != AgentState::LoggedOut;
                if (!videoAgentOnline || sched_.now() - call.queuedAt >= videoFallbackWait_)
                    chosen = pick([](const Agent&) { return true; });
            }
        } else {
            chosen = pick([](const Agent& a) { return !a.cfg.video; });
            if (!chosen) chosen = pick([](const Agent&) { return true; });
        }
        if (!chosen) {
            ++it;
            continue;
        }
        it = queue_.erase(it);
        if (videoCaller && !chosen->cfg.video)
            note(call.id + " no video agent within " + std::to_string(videoFallbackWait_ / 1'000'000) +
                 " s: falling back to audio agent " + chosen->cfg.ext);
        offer(call, *chosen);
    }
}

void ContactCenter::offer(CcCall& call, Agent& agent) {
    agent.state = AgentState::Busy;
    call.agentExt = agent.cfg.ext;
    auto leg = newLeg(true);
    leg->dialog.callId = call.id + "-" + agent.cfg.ext + "-" + std::to_string(nextPort_) + "@" + address_;
    leg->dialog.localTag = "cc" + call.id + "b" + std::to_string(nextPort_);
    leg->dialog.localUri = "sip:" + call.callerUser + "@" + address_;
    leg->dialog.remoteUri = "sip:" + agent.cfg.ext + "@" + agent.cfg.sipAddress;
    leg->dialog.remoteAddr = agent.cfg.sipAddress;
    leg->dialog.localSeq = 0;
    bool video = agent.cfg.video && (call.outbound || call.callerCanVideo);
    leg->local = makeOffer(caps_, address_, leg->ports, video);
    leg->local.user = "cc";
    SipMessage inv = leg->dialog.request("INVITE");
    inv.add("Contact", "<sip:" + call.callerUser + "@" + address_ + ">");
    if (agent.cfg.onPbx) inv.add("User-to-User", call.id);
    if (call.outbound) inv.add("Call-Info", "<sip:" + address_ + ">;answer-after=0");
    inv.add("Content-Type", "application/sdp");
    inv.body = leg->local.toString();
    sendOnLeg(*leg, inv);
    note(call.id + " offered to " + agent.cfg.ext + " (" + agent.cfg.name + (agent.cfg.onPbx ? ", PBX desk phone" : "") +
         "): INVITE with " + (video ? "audio+video" : "audio only"));
    legIndex_[leg->dialog.callId] = {call.id, false};
    call.agent = std::move(leg);
}

void ContactCenter::agentAnswered(CcCall& call) {
    Agent& a = agents_[call.agentExt];
    a.state = AgentState::Busy;
    CallRecord& rec = record(call);
    if (rec.answered < 0) {
        rec.answered = sched_.now();
        rec.videoAtFirstAnswer = call.agent->video();
    }
    rec.agents.push_back(call.agentExt);
    EstablishedEvent e;
    e.establishedConnection = {call.id, call.agentExt};
    e.answeringDevice = call.agentExt;
    e.callingDevice = call.callerUser;
    e.calledDevice = call.dialed;
    e.video = call.agent->video();
    emit(e);
    note(call.id + " answered by " + call.agentExt + (call.agent->video() ? " with video" : " audio only"));
    call.announcer.reset();
    alignVideoAndBridge(call);
}

void ContactCenter::alignVideoAndBridge(CcCall& call) {
    bool want = call.agent->video();
    bool have = call.caller->video();
    if (want != have && (!want || call.callerCanVideo)) {
        ++record(call).videoReinvites;
        note(call.id + (want ? " re-INVITE to caller: add video" : " re-INVITE to caller: drop video (agent has none)"));
        std::string id = call.id;
        reoffer(call, *call.caller, want, Direction::SendRecv, [this, id]() {
            auto it = calls_.find(id);
            if (it != calls_.end()) bridge(*it->second);
        });
        return;
    }
    bridge(call);
}

void ContactCenter::reoffer(CcCall& call, Leg& leg, bool video, Direction dir, std::function<void()> then) {
    (void)call;
    leg.pendingOffer = makeReoffer(leg.local, caps_, leg.ports, video, dir);
    leg.reofferPending = true;
    leg.afterReoffer = std::move(then);
    SipMessage inv = leg.dialog.request("INVITE");
    inv.add("Content-Type", "application/sdp");
    inv.body = leg.pendingOffer.toString();
    sendOnLeg(leg, inv);
}

void ContactCenter::bridge(CcCall& call) {
    if (!call.caller || !call.agent || !call.caller->established || !call.agent->established) return;
    if (call.relay) relayedFinished_ += call.relay->forwarded();
    call.relay = std::make_unique<MediaRelay>(net_, address_);
    std::string kinds;
    for (const char* type : {"audio", "video"}) {
        const NegotiatedStream* a = call.caller->stream(type);
        const NegotiatedStream* b = call.agent->stream(type);
        if (!a || !b) continue;
        int pa = std::string(type) == "audio" ? call.caller->ports.audio : call.caller->ports.video;
        int pb = std::string(type) == "audio" ? call.agent->ports.audio : call.agent->ports.video;
        call.relay->connect(pa, {a->remoteAddress, a->remotePort}, pb, {b->remoteAddress, b->remotePort});
        kinds += kinds.empty() ? type : std::string("+") + type;
    }
    note(call.id + " media relayed " + (call.outbound ? call.agentExt + " <-> " + call.outboundNumber
                                                    : call.callerUser + " <-> " + call.agentExt) +
         ": " + (kinds.empty() ? "none" : kinds));
}

void ContactCenter::setReady(Agent& a) {
    a.state = AgentState::Ready;
    a.idleSince = sched_.now();
    emit(AgentReadyEvent{"", a.cfg.ext});
}

void ContactCenter::dropAgentLeg(CcCall& call, const std::string& cause, bool sendBye) {
    if (!call.agent) return;
    Leg& leg = *call.agent;
    if (sendBye) {
        if (leg.established) {
            sendOnLeg(leg, leg.dialog.request("BYE"));
        } else {  // still ringing: CANCEL reuses the INVITE's CSeq number (RFC 3261 9.1)
            SipMessage cancel;
            cancel.method = "CANCEL";
            cancel.uri = leg.dialog.remoteUri;
            cancel.add("From", "<" + leg.dialog.localUri + ">;tag=" + leg.dialog.localTag);
            cancel.add("To", "<" + leg.dialog.remoteUri + ">");
            cancel.add("Call-ID", leg.dialog.callId);
            cancel.add("CSeq", "1 CANCEL");
            sendOnLeg(leg, cancel);
        }
    }
    legIndex_.erase(leg.dialog.callId);
    ConnectionClearedEvent e;
    e.droppedConnection = {call.id, call.agentExt};
    e.releasingDevice = sendBye ? call.callerUser : call.agentExt;
    e.cause = cause;
    emit(e);
    if (call.relay) {
        relayedFinished_ += call.relay->forwarded();
        call.relay.reset();
    }
    std::string ext = call.agentExt;
    call.agent.reset();
    call.agentExt.clear();
    setReady(agents_[ext]);
}

void ContactCenter::endCall(CcCall& call, const std::string& outcome, bool callerAlreadyGone, bool agentAlreadyGone) {
    std::string id = call.id;
    if (call.agent) dropAgentLeg(call, "normalClearing", !agentAlreadyGone);
    if (call.caller) {
        if (!callerAlreadyGone && call.caller->established) sendOnLeg(*call.caller, call.caller->dialog.request("BYE"));
        legIndex_.erase(call.caller->dialog.callId);
    }
    bool wasQueued = false;
    for (auto it = queue_.begin(); it != queue_.end(); ++it)
        if (*it == id) {
            queue_.erase(it);
            wasQueued = true;
            break;
        }
    CallRecord& rec = record(call);
    rec.ended = sched_.now();
    rec.outcome = wasQueued ? "abandoned" : outcome;
    if (call.relay) relayedFinished_ += call.relay->forwarded();
    note(id + " ended: " + rec.outcome);
    auto it = calls_.find(id);
    graveyard_.push_back(std::move(it->second));
    calls_.erase(it);
    route();
}

// ------------------------------------------------------------------ CSTA side

CstaMessage ContactCenter::handle(const CstaMessage& request) {
    graveyard_.clear();
    auto findCall = [this](const std::string& id) -> CcCall* {
        auto it = calls_.find(id);
        return it == calls_.end() ? nullptr : it->second.get();
    };

    if (auto* r = std::get_if<AnswerCall>(&request)) {
        CcCall* call = findCall(r->callToBeAnswered.callId);
        if (!call) return error("invalidCallID");
        if (!call->agent || call->agentExt != r->callToBeAnswered.deviceId) return error("invalidConnectionID");
        if (!call->agent->ringing) return error("invalidObjectState");
        const Agent& a = agents_[call->agentExt];
        if (a.cfg.onPbx) {
            if (!pbx_ || call->pbxCallId.empty()) return error("invalidObjectState");
            pbx_->request(AnswerCall{{call->pbxCallId, call->agentExt}});
            note(call->id + " AnswerCall(" + call->agentExt + ") forwarded to the PBX as AnswerCall(" + call->pbxCallId + ")");
        } else {
            SipMessage n = call->agent->dialog.request("NOTIFY");
            n.add("Event", "talk");
            n.add("Subscription-State", "terminated");
            sendOnLeg(*call->agent, n);
            note(call->id + " AnswerCall(" + call->agentExt + ") mapped to NOTIFY Event: talk");
        }
        return AnswerCallResponse{};
    }
    if (auto* r = std::get_if<ClearConnection>(&request)) {
        CcCall* call = findCall(r->connectionToBeCleared.callId);
        if (!call) return error("invalidCallID");
        const std::string& dev = r->connectionToBeCleared.deviceId;
        if (dev != call->agentExt && dev != call->callerUser) return error("invalidConnectionID");
        note(call->id + " ClearConnection(" + dev + ") mapped to BYE on both legs");
        endCall(*call, "completed", false, false);
        return ClearConnectionResponse{};
    }
    if (auto* r = std::get_if<SingleStepTransferCall>(&request)) {
        CcCall* call = findCall(r->activeCall.callId);
        if (!call) return error("invalidCallID");
        if (!call->agent || call->agentExt != r->activeCall.deviceId || !call->agent->established)
            return error("invalidObjectState");
        auto target = agents_.find(r->transferredTo);
        if (target == agents_.end()) return error("invalidDeviceID");
        if (target->second.state != AgentState::Ready) return error("invalidObjectState");
        ++record(*call).transfers;
        note(call->id + " SingleStepTransferCall " + call->agentExt + " -> " + r->transferredTo +
             ": BYE to the old agent, INVITE to the new one");
        dropAgentLeg(*call, "transferred", true);
        offer(*call, target->second);
        std::string id = call->id;
        route();
        return SingleStepTransferCallResponse{{id, r->transferredTo}};
    }
    if (auto* r = std::get_if<MakeCall>(&request)) {
        auto a = agents_.find(r->callingDevice);
        if (a == agents_.end()) return error("invalidDeviceID");
        if (a->second.state != AgentState::Ready) return error("invalidObjectState");
        if (!directory_.count(r->calledDirectoryNumber)) return error("invalidCalledDeviceID");
        auto call = std::make_unique<CcCall>();
        call->id = "C" + std::to_string(++nextCall_);
        call->outbound = true;
        call->outboundNumber = r->calledDirectoryNumber;
        call->callerUser = r->callingDevice;
        call->dialed = r->calledDirectoryNumber;
        call->recordIndex = records_.size();
        CallRecord rec;
        rec.callId = call->id;
        rec.caller = r->callingDevice;
        rec.outbound = true;
        rec.arrived = sched_.now();
        records_.push_back(rec);
        note(call->id + " MakeCall(" + r->callingDevice + " -> " + r->calledDirectoryNumber +
             "): INVITE to the agent phone with answer-after=0");
        CcCall& ref = *call;
        calls_[call->id] = std::move(call);
        offer(ref, a->second);
        return MakeCallResponse{{ref.id, r->callingDevice}};
    }
    if (auto* r = std::get_if<SetAgentState>(&request)) {
        auto it = agents_.find(r->device);
        if (it == agents_.end()) return error("invalidDeviceID");
        Agent& a = it->second;
        if (a.state == AgentState::Busy && r->requestedAgentState != "notReady") return error("invalidObjectState");
        if (r->requestedAgentState == "loggedOn") {
            a.state = AgentState::NotReady;
        } else if (r->requestedAgentState == "loggedOff") {
            a.state = AgentState::LoggedOut;
        } else if (r->requestedAgentState == "ready") {
            setReady(a);
            route();
        } else {
            if (a.state != AgentState::Busy) a.state = AgentState::NotReady;
            emit(AgentNotReadyEvent{"", a.cfg.ext});
        }
        return SetAgentStateResponse{};
    }
    return error("requestIncompatibleWithObject");
}

void ContactCenter::attachPbx(CstaClient* link) {
    pbx_ = link;
    for (const auto& kv : agents_)
        if (kv.second.cfg.onPbx) pbx_->request(MonitorStart{kv.first});
}

void ContactCenter::onPbxEvent(const CstaMessage& event) {
    graveyard_.clear();
    if (auto* d = std::get_if<DeliveredEvent>(&event)) {
        auto it = calls_.find(d->correlatorData);
        if (it != calls_.end()) {
            it->second->pbxCallId = d->connection.callId;
            note(it->second->id + " PBX reports DeliveredEvent " + d->connection.callId + " at " + d->alertingDevice +
                 " (correlated by user-to-user data)");
        }
    } else if (auto* r = std::get_if<AgentReadyEvent>(&event)) {
        auto it = agents_.find(r->agentDevice);
        if (it != agents_.end() && it->second.cfg.onPbx && it->second.state != AgentState::Busy) {
            note("PBX reports agent " + r->agentDevice + " ready");
            setReady(it->second);
            route();
        }
    } else if (auto* n = std::get_if<AgentNotReadyEvent>(&event)) {
        auto it = agents_.find(n->agentDevice);
        if (it != agents_.end() && it->second.cfg.onPbx && it->second.state != AgentState::Busy) {
            it->second.state = AgentState::NotReady;
            emit(AgentNotReadyEvent{"", n->agentDevice});
        }
    }
}

}  // namespace svcc
