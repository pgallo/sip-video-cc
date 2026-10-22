// CSTA over TCP (POSIX sockets). Both ends are single-threaded and driven by poll(), so they
// can be pumped from the same loop as the simulation without locks.
#pragma once

#include "svcc/csta.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace svcc {

// What a CSTA server needs from whatever it fronts (the contact-center core, or a PBX).
class CstaProvider {
public:
    virtual ~CstaProvider() = default;
    virtual bool hasDevice(const std::string& device) const = 0;
    virtual CstaMessage handle(const CstaMessage& request) = 0;
};

class CstaServer {
public:
    explicit CstaServer(CstaProvider& provider, std::uint16_t port = 0);
    ~CstaServer();
    CstaServer(const CstaServer&) = delete;
    CstaServer& operator=(const CstaServer&) = delete;

    std::uint16_t port() const { return port_; }
    // Accepts, reads and dispatches; returns the number of frames and connections handled.
    int poll(int timeoutMs);
    // Sends an event to every session that monitors the event's device, with that session's cross-reference id.
    void publish(const CstaMessage& event);
    std::size_t sessions() const { return sessions_.size(); }

private:
    struct Session {
        int fd;
        FrameDecoder decoder;
        std::map<std::string, std::string> monitors;  // device -> cross-reference id
    };
    void handleFrame(Session& s, int invokeId, const std::string& xml);
    void sendTo(Session& s, int invokeId, const CstaMessage& m);

    CstaProvider& provider_;
    int listenFd_ = -1;
    std::uint16_t port_ = 0;
    int nextCrossRef_ = 0;
    std::vector<std::unique_ptr<Session>> sessions_;
};

class CstaClient {
public:
    explicit CstaClient(std::uint16_t port);
    ~CstaClient();
    CstaClient(const CstaClient&) = delete;
    CstaClient& operator=(const CstaClient&) = delete;

    // Sends a request; onResponse is called with the correlated response (or CSTAErrorCode).
    int request(const CstaMessage& m, std::function<void(const CstaMessage&)> onResponse = {});
    int poll(int timeoutMs);
    std::size_t pending() const { return pending_.size(); }

    std::function<void(const CstaMessage&)> onEvent;

private:
    int fd_ = -1;
    int nextInvoke_ = 0;
    FrameDecoder decoder_;
    std::map<int, std::function<void(const CstaMessage&)>> pending_;
};

}  // namespace svcc
