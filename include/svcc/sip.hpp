// Minimal SIP: message model, parser and serializer, and an in-process network that
// serializes every message on send and parses it on delivery. Transport is reliable and
// ordered, so there are no retransmission timers here (the companion Java repository covers
// the full RFC 3261 transaction layer).
#pragma once

#include "svcc/sim.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace svcc {

struct SipError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct SipMessage {
    bool isRequest = true;
    std::string method;
    std::string uri;
    int status = 0;
    std::string reason;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    std::string header(const std::string& name) const;
    void set(const std::string& name, const std::string& value);
    void add(const std::string& name, const std::string& value);

    std::string callId() const { return header("Call-ID"); }
    int cseq() const;
    std::string cseqMethod() const;
    std::string fromTag() const;
    std::string toTag() const;

    std::string serialize() const;
    static SipMessage parse(const std::string& text);
};

SipMessage makeResponse(const SipMessage& req, int status, const std::string& reason,
                        const std::string& toTag = "");
std::string uriUser(const std::string& uri);
std::string uriHost(const std::string& uri);
std::string headerParam(const std::string& value, const std::string& name);

class SipEndpoint {
public:
    virtual ~SipEndpoint() = default;
    virtual void onSip(const SipMessage& msg, const std::string& from) = 0;
};

class SipNetwork {
public:
    SipNetwork(Scheduler& sched, Micros latency) : sched_(sched), latency_(latency) {}
    void attach(const std::string& address, SipEndpoint* endpoint) { endpoints_[address] = endpoint; }
    void send(const std::string& from, const std::string& to, const SipMessage& msg);

    bool tracing = false;
    std::vector<std::string> trace;
    std::uint64_t delivered = 0;

private:
    Scheduler& sched_;
    Micros latency_;
    std::map<std::string, SipEndpoint*> endpoints_;
};

// One side of a dialog, enough to build in-dialog requests.
struct Dialog {
    std::string callId;
    std::string localTag;
    std::string remoteTag;
    std::string localUri;
    std::string remoteUri;
    std::string remoteAddr;
    int localSeq = 1;

    SipMessage request(const std::string& method) {
        SipMessage m;
        m.method = method;
        m.uri = remoteUri;
        m.add("From", "<" + localUri + ">;tag=" + localTag);
        m.add("To", "<" + remoteUri + ">" + (remoteTag.empty() ? "" : ";tag=" + remoteTag));
        m.add("Call-ID", callId);
        m.add("CSeq", std::to_string(++localSeq) + " " + method);
        return m;
    }
};

}  // namespace svcc
