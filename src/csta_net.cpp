#include "svcc/csta_net.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace svcc {

namespace {

void sendAll(int fd, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;  // peer gone; the read side will notice
        }
        off += static_cast<std::size_t>(n);
    }
}

// Returns bytes read, 0 on orderly close, -1 if nothing available.
ssize_t readSome(int fd, FrameDecoder& dec) {
    char buf[8192];
    ssize_t n = ::recv(fd, buf, sizeof buf, MSG_DONTWAIT);
    if (n > 0) dec.feed(buf, static_cast<std::size_t>(n));
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return -1;
    return n;
}

}  // namespace

// ------------------------------------------------------------------ server

CstaServer::CstaServer(CstaProvider& provider, std::uint16_t port) : provider_(provider) {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) throw std::runtime_error("socket failed");
    int one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 || ::listen(listenFd_, 16) < 0)
        throw std::runtime_error(std::string("CSTA server bind/listen failed: ") + std::strerror(errno));
    socklen_t len = sizeof a;
    ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
}

CstaServer::~CstaServer() {
    for (auto& s : sessions_) ::close(s->fd);
    ::close(listenFd_);
}

int CstaServer::poll(int timeoutMs) {
    std::vector<pollfd> fds;
    fds.push_back({listenFd_, POLLIN, 0});
    for (auto& s : sessions_) fds.push_back({s->fd, POLLIN, 0});
    if (::poll(fds.data(), fds.size(), timeoutMs) <= 0) return 0;
    int activity = 0;
    if (fds[0].revents & POLLIN) {
        int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd >= 0) {
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            sessions_.push_back(std::make_unique<Session>(Session{fd, {}, {}}));
            ++activity;
        }
    }
    for (std::size_t i = 1; i < fds.size(); ++i) {
        if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        Session* s = nullptr;
        for (auto& p : sessions_)
            if (p->fd == fds[i].fd) s = p.get();
        if (!s) continue;
        ssize_t n = readSome(s->fd, s->decoder);
        if (n == 0) {
            ::close(s->fd);
            s->fd = -1;
            continue;
        }
        int id;
        std::string xml;
        try {
            while (s->decoder.next(id, xml)) {
                handleFrame(*s, id, xml);
                ++activity;
            }
        } catch (const CstaDecodeError&) {
            ::close(s->fd);  // framing is lost; drop the session
            s->fd = -1;
        }
    }
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [](const auto& s) { return s->fd < 0; }),
                    sessions_.end());
    return activity;
}

void CstaServer::handleFrame(Session& s, int invokeId, const std::string& xml) {
    CstaMessage request;
    try {
        request = decodeXml(xml);
    } catch (const CstaDecodeError&) {
        sendTo(s, invokeId, CstaErrorCode{"invalidParameterValue"});
        return;
    }
    if (isEvent(request)) {
        sendTo(s, invokeId, CstaErrorCode{"requestIncompatibleWithObject"});
        return;
    }
    if (auto* ms = std::get_if<MonitorStart>(&request)) {
        if (!provider_.hasDevice(ms->device)) {
            sendTo(s, invokeId, CstaErrorCode{"invalidDeviceID"});
            return;
        }
        std::string ref = "M" + std::to_string(++nextCrossRef_);
        s.monitors[ms->device] = ref;
        sendTo(s, invokeId, MonitorStartResponse{ref});
        return;
    }
    sendTo(s, invokeId, provider_.handle(request));
}

void CstaServer::sendTo(Session& s, int invokeId, const CstaMessage& m) {
    if (s.fd >= 0) sendAll(s.fd, frame(invokeId, encodeXml(m)));
}

void CstaServer::publish(const CstaMessage& event) {
    std::string device = eventDevice(event);
    for (auto& s : sessions_) {
        auto it = s->monitors.find(device);
        if (it == s->monitors.end()) continue;
        CstaMessage copy = event;
        setCrossRef(copy, it->second);
        sendTo(*s, kEventInvokeId, copy);
    }
}

// ------------------------------------------------------------------ client

CstaClient::CstaClient(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (fd_ < 0 || ::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0)
        throw std::runtime_error(std::string("CSTA connect failed: ") + std::strerror(errno));
    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

CstaClient::~CstaClient() {
    if (fd_ >= 0) ::close(fd_);
}

int CstaClient::request(const CstaMessage& m, std::function<void(const CstaMessage&)> onResponse) {
    nextInvoke_ = nextInvoke_ % 9998 + 1;
    pending_[nextInvoke_] = std::move(onResponse);
    sendAll(fd_, frame(nextInvoke_, encodeXml(m)));
    return nextInvoke_;
}

int CstaClient::poll(int timeoutMs) {
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, timeoutMs) <= 0) return 0;
    if (readSome(fd_, decoder_) == 0) return 0;
    int activity = 0;
    int id;
    std::string xml;
    while (decoder_.next(id, xml)) {
        ++activity;
        CstaMessage m = decodeXml(xml);
        if (id == kEventInvokeId && isEvent(m)) {
            if (onEvent) onEvent(m);
            continue;
        }
        auto it = pending_.find(id);
        if (it == pending_.end()) continue;
        auto cb = std::move(it->second);
        pending_.erase(it);
        if (cb) cb(m);
    }
    return activity;
}

}  // namespace svcc
