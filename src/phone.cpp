#include "svcc/phone.hpp"

#include <functional>

namespace svcc {

namespace {

std::string stripAngles(const std::string& v) {
    auto lt = v.find('<');
    auto gt = v.find('>');
    if (lt == std::string::npos || gt == std::string::npos) return v.substr(0, v.find(';'));
    return v.substr(lt + 1, gt - lt - 1);
}

}  // namespace

SipPhone::SipPhone(Scheduler& sched, SipNetwork& sip, MediaNetwork& net, std::string address, std::string user,
                   Capabilities caps, int basePort)
    : sched_(sched), sip_(sip), net_(net), address_(std::move(address)), user_(std::move(user)),
      caps_(std::move(caps)), basePort_(basePort) {}

std::string SipPhone::call(const std::string& targetUri, const std::string& targetAddress, bool video) {
    int n = ++seq_;
    std::string callId = user_ + "-" + std::to_string(n) + "@" + address_;
    Call& c = calls_[callId];
    c.outgoing = true;
    c.ports = {basePort_ + 4 * n, basePort_ + 4 * n + 2};
    c.media = std::make_unique<MediaSession>(sched_, net_, address_, c.ports,
                                             static_cast<std::uint32_t>(std::hash<std::string>{}(callId)));
    c.dialog.callId = callId;
    c.dialog.localTag = user_ + "t" + std::to_string(n);
    c.dialog.localUri = "sip:" + user_ + "@" + address_;
    c.dialog.remoteUri = targetUri;
    c.dialog.remoteAddr = targetAddress;
    c.dialog.localSeq = 0;
    c.local = makeOffer(caps_, address_, c.ports, video);
    c.local.user = user_;
    SipMessage inv = c.dialog.request("INVITE");
    inv.add("Contact", "<sip:" + user_ + "@" + address_ + ">");
    inv.add("Content-Type", "application/sdp");
    inv.body = c.local.toString();
    send(c, inv);
    return callId;
}

void SipPhone::answer(const std::string& callId) {
    auto it = calls_.find(callId);
    if (it == calls_.end() || !it->second.ringing) return;
    Call& c = it->second;
    c.ringing = false;
    c.awaitingAck = true;
    c.local = makeAnswer(c.remote, caps_, address_, c.ports);
    c.local.user = user_;
    SipMessage ok = makeResponse(c.invite, 200, "OK", c.dialog.localTag);
    ok.add("Contact", "<sip:" + user_ + "@" + address_ + ">");
    ok.add("Content-Type", "application/sdp");
    ok.body = c.local.toString();
    send(c, ok);
}

void SipPhone::hangup(const std::string& callId) {
    auto it = calls_.find(callId);
    if (it == calls_.end()) return;
    Call& c = it->second;
    if (c.ringing) {
        send(c, makeResponse(c.invite, 603, "Decline", c.dialog.localTag));
    } else {
        send(c, c.dialog.request("BYE"));
    }
    end(callId);
}

void SipPhone::hangupAll() {
    for (const auto& id : callIds()) hangup(id);
}

void SipPhone::reinvite(const std::string& callId, bool video, bool hold) {
    auto it = calls_.find(callId);
    if (it == calls_.end() || !it->second.established) return;
    Call& c = it->second;
    c.pendingOffer = makeReoffer(c.local, caps_, c.ports, video, hold ? Direction::SendOnly : Direction::SendRecv);
    c.reofferPending = true;
    SipMessage inv = c.dialog.request("INVITE");
    inv.add("Contact", "<sip:" + user_ + "@" + address_ + ">");
    inv.add("Content-Type", "application/sdp");
    inv.body = c.pendingOffer.toString();
    send(c, inv);
}

void SipPhone::onSip(const SipMessage& m, const std::string& from) {
    if (m.isRequest)
        onRequest(m, from);
    else
        onResponse(m);
}

void SipPhone::onRequest(const SipMessage& m, const std::string& from) {
    if (m.method == "INVITE" && m.toTag().empty()) {
        int n = ++seq_;
        Call& c = calls_[m.callId()];
        c.invite = m;
        c.ringing = true;
        c.ports = {basePort_ + 4 * n, basePort_ + 4 * n + 2};
        c.media = std::make_unique<MediaSession>(sched_, net_, address_, c.ports,
                                                 static_cast<std::uint32_t>(std::hash<std::string>{}(m.callId() + user_)));
        c.dialog.callId = m.callId();
        c.dialog.localTag = user_ + "t" + std::to_string(n);
        c.dialog.remoteTag = m.fromTag();
        c.dialog.localUri = stripAngles(m.header("To"));
        c.dialog.remoteUri = stripAngles(m.header("From"));
        c.dialog.remoteAddr = from;
        c.dialog.localSeq = 100;
        try {
            c.remote = SessionDescription::parse(m.body);
        } catch (const SdpError&) {
            send(c, makeResponse(m, 400, "Bad Request", c.dialog.localTag));
            calls_.erase(m.callId());
            return;
        }
        if (!caps_.audio.empty() && makeAnswer(c.remote, caps_, address_, c.ports).media.front().port == 0) {
            send(c, makeResponse(m, 488, "Not Acceptable Here", c.dialog.localTag));
            calls_.erase(m.callId());
            return;
        }
        SipMessage ringing = makeResponse(m, 180, "Ringing", c.dialog.localTag);
        send(c, ringing);
        std::string callId = m.callId();
        if (onIncoming) onIncoming(callId, m);
        if (autoAnswer || m.header("Call-Info").find("answer-after=0") != std::string::npos) answer(callId);
        return;
    }
    auto it = calls_.find(m.callId());
    if (it == calls_.end()) {
        if (m.method != "ACK") sip_.send(address_, from, makeResponse(m, 481, "Call/Transaction Does Not Exist"));
        return;
    }
    Call& c = it->second;
    if (m.method == "ACK") {
        if (c.awaitingAck) {
            c.awaitingAck = false;
            bool first = !c.established;
            c.established = true;
            c.streams = negotiate(c.local, c.remote, c.local);
            apply(c);
            if (first && onEstablished) onEstablished(c.dialog.callId);
        }
    } else if (m.method == "INVITE") {  // re-INVITE: answer with the same capabilities
        c.remote = SessionDescription::parse(m.body);
        c.local = makeAnswer(c.remote, caps_, address_, c.ports);
        c.local.user = user_;
        c.awaitingAck = true;
        SipMessage ok = makeResponse(m, 200, "OK");
        ok.add("Contact", "<sip:" + user_ + "@" + address_ + ">");
        ok.add("Content-Type", "application/sdp");
        ok.body = c.local.toString();
        send(c, ok);
    } else if (m.method == "BYE") {
        send(c, makeResponse(m, 200, "OK"));
        end(m.callId());
    } else if (m.method == "CANCEL") {
        send(c, makeResponse(m, 200, "OK"));
        if (c.ringing) {
            send(c, makeResponse(c.invite, 487, "Request Terminated", c.dialog.localTag));
            end(m.callId());
        }
    } else if (m.method == "NOTIFY") {
        send(c, makeResponse(m, 200, "OK"));
        if (m.header("Event") == "talk" && c.ringing) answer(m.callId());
    } else {
        send(c, makeResponse(m, 501, "Not Implemented"));
    }
}

void SipPhone::onResponse(const SipMessage& m) {
    auto it = calls_.find(m.callId());
    if (it == calls_.end() || m.cseqMethod() != "INVITE") return;
    Call& c = it->second;
    if (m.status < 200) {
        if (!m.toTag().empty()) c.dialog.remoteTag = m.toTag();
        return;
    }
    if (m.status >= 300) {
        if (c.reofferPending) {
            c.reofferPending = false;  // keep the current session
        } else if (!c.established) {
            end(m.callId());
        }
        return;
    }
    c.dialog.remoteTag = m.toTag();
    SipMessage a;
    a.method = "ACK";
    a.uri = c.dialog.remoteUri;
    a.add("From", "<" + c.dialog.localUri + ">;tag=" + c.dialog.localTag);
    a.add("To", "<" + c.dialog.remoteUri + ">;tag=" + c.dialog.remoteTag);
    a.add("Call-ID", c.dialog.callId);
    a.add("CSeq", std::to_string(m.cseq()) + " ACK");
    send(c, a);
    if (c.reofferPending) {
        c.reofferPending = false;
        c.local = c.pendingOffer;
    } else if (c.established) {
        return;  // retransmitted 200
    }
    c.remote = SessionDescription::parse(m.body);
    c.streams = negotiate(c.local, c.remote, c.remote);
    bool first = !c.established;
    c.established = true;
    apply(c);
    if (first && onEstablished) onEstablished(c.dialog.callId);
}

void SipPhone::apply(Call& c) { c.media->apply(c.streams); }

void SipPhone::end(const std::string& callId) {
    auto it = calls_.find(callId);
    if (it == calls_.end()) return;
    it->second.media->stop();
    calls_.erase(it);
    if (onEnded) onEnded(callId);
}

const SipPhone::Call* SipPhone::find(const std::string& callId) const {
    auto it = calls_.find(callId);
    return it == calls_.end() ? nullptr : &it->second;
}

std::vector<std::string> SipPhone::callIds() const {
    std::vector<std::string> ids;
    for (const auto& kv : calls_) ids.push_back(kv.first);
    return ids;
}

bool SipPhone::videoActive(const std::string& callId) const {
    const Call* c = find(callId);
    if (!c) return false;
    for (const auto& s : c->streams)
        if (s.type == "video" && s.active) return true;
    return false;
}

}  // namespace svcc
