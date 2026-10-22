// A SIP user agent with audio and optional video: places and answers calls, answers re-INVITEs,
// adds or removes video and holds with re-INVITE, and can be answered remotely (NOTIFY with
// Event: talk, or an answer-after=0 Call-Info parameter) which is how third-party call control
// reaches a SIP phone.
#pragma once

#include "svcc/media.hpp"
#include "svcc/sdp.hpp"
#include "svcc/sip.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace svcc {

class SipPhone : public SipEndpoint {
public:
    struct Call {
        Dialog dialog;
        bool outgoing = false;
        bool ringing = false;
        bool established = false;
        bool awaitingAck = false;
        SipMessage invite;
        SessionDescription local;
        SessionDescription remote;
        SessionDescription pendingOffer;
        bool reofferPending = false;
        std::vector<NegotiatedStream> streams;
        MediaPorts ports{0, 0};
        std::unique_ptr<MediaSession> media;
    };

    SipPhone(Scheduler& sched, SipNetwork& sip, MediaNetwork& net, std::string address, std::string user,
             Capabilities caps, int basePort);

    std::string call(const std::string& targetUri, const std::string& targetAddress, bool video);
    void answer(const std::string& callId);
    void hangup(const std::string& callId);
    void hangupAll();
    void reinvite(const std::string& callId, bool video, bool hold);

    void onSip(const SipMessage& msg, const std::string& from) override;

    const Call* find(const std::string& callId) const;
    std::vector<std::string> callIds() const;
    bool videoActive(const std::string& callId) const;
    const std::string& address() const { return address_; }
    const std::string& user() const { return user_; }
    const Capabilities& capabilities() const { return caps_; }

    bool autoAnswer = false;
    std::function<void(const std::string& callId, const SipMessage& invite)> onIncoming;
    std::function<void(const std::string& callId)> onEstablished;
    std::function<void(const std::string& callId)> onEnded;

private:
    void onRequest(const SipMessage& m, const std::string& from);
    void onResponse(const SipMessage& m);
    void apply(Call& c);
    void end(const std::string& callId);
    void send(Call& c, const SipMessage& m) { sip_.send(address_, c.dialog.remoteAddr, m); }

    Scheduler& sched_;
    SipNetwork& sip_;
    MediaNetwork& net_;
    std::string address_;
    std::string user_;
    Capabilities caps_;
    int basePort_;
    int seq_ = 0;
    std::map<std::string, Call> calls_;
};

}  // namespace svcc
