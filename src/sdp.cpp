#include "svcc/sdp.hpp"

#include <algorithm>
#include <sstream>

namespace svcc {

const char* toString(Direction d) {
    switch (d) {
        case Direction::SendRecv: return "sendrecv";
        case Direction::SendOnly: return "sendonly";
        case Direction::RecvOnly: return "recvonly";
        case Direction::Inactive: return "inactive";
    }
    return "sendrecv";
}

Direction answerDirection(Direction offered, bool localHold) {
    switch (offered) {
        case Direction::SendOnly: return Direction::RecvOnly;
        case Direction::RecvOnly: return localHold ? Direction::Inactive : Direction::SendOnly;
        case Direction::Inactive: return Direction::Inactive;
        case Direction::SendRecv: return localHold ? Direction::SendOnly : Direction::SendRecv;
    }
    return Direction::SendRecv;
}

std::optional<Codec> staticCodec(int pt) {
    for (const Codec& c : {codecs::PCMU, codecs::PCMA, codecs::H261, codecs::H263})
        if (c.pt == pt) return c;
    return std::nullopt;
}

const MediaDescription* SessionDescription::find(const std::string& type) const {
    for (const auto& m : media)
        if (m.type == type) return &m;
    return nullptr;
}

MediaDescription* SessionDescription::find(const std::string& type) {
    for (auto& m : media)
        if (m.type == type) return &m;
    return nullptr;
}

std::string SessionDescription::toString() const {
    std::ostringstream o;
    o << "v=0\r\n"
      << "o=" << user << ' ' << sessionId << ' ' << version << " IN IP4 " << address << "\r\n"
      << "s=-\r\n"
      << "c=IN IP4 " << address << "\r\n"
      << "t=0 0\r\n";
    for (const auto& m : media) {
        o << "m=" << m.type << ' ' << m.port << " RTP/AVP";
        for (int pt : m.payloads) o << ' ' << pt;
        o << "\r\n";
        for (int pt : m.payloads)
            if (auto c = staticCodec(pt)) o << "a=rtpmap:" << pt << ' ' << c->name << '/' << c->clockRate << "\r\n";
        o << "a=" << svcc::toString(m.dir) << "\r\n";
    }
    return o.str();
}

SessionDescription SessionDescription::parse(const std::string& text) {
    SessionDescription s;
    std::istringstream in(text);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size() < 2 || line[1] != '=') throw SdpError("malformed line: " + line);
        if (first && line != "v=0") throw SdpError("SDP must start with v=0");
        first = false;
        char type = line[0];
        std::istringstream f(line.substr(2));
        if (type == 'o') {
            std::string in_, ip4;
            if (!(f >> s.user >> s.sessionId >> s.version >> in_ >> ip4 >> s.address)) throw SdpError("bad o= line");
        } else if (type == 'c') {
            std::string in_, ip4;
            f >> in_ >> ip4 >> s.address;
        } else if (type == 'm') {
            MediaDescription m;
            std::string proto;
            if (!(f >> m.type >> m.port >> proto) || proto != "RTP/AVP") throw SdpError("bad m= line: " + line);
            if (m.port < 0 || m.port > 65535) throw SdpError("port out of range");
            int pt;
            while (f >> pt) m.payloads.push_back(pt);
            if (m.payloads.empty()) throw SdpError("m= line without formats");
            s.media.push_back(m);
        } else if (type == 'a' && !s.media.empty()) {
            std::string a = line.substr(2);
            if (a == "sendrecv") s.media.back().dir = Direction::SendRecv;
            else if (a == "sendonly") s.media.back().dir = Direction::SendOnly;
            else if (a == "recvonly") s.media.back().dir = Direction::RecvOnly;
            else if (a == "inactive") s.media.back().dir = Direction::Inactive;
        }
    }
    if (first) throw SdpError("empty SDP");
    return s;
}

namespace {

std::vector<int> pts(const std::vector<Codec>& cs) {
    std::vector<int> out;
    for (const auto& c : cs) out.push_back(c.pt);
    return out;
}

const std::vector<Codec>& capsFor(const Capabilities& caps, const std::string& type) {
    static const std::vector<Codec> none;
    if (type == "audio") return caps.audio;
    if (type == "video") return caps.video;
    return none;
}

}  // namespace

SessionDescription makeOffer(const Capabilities& caps, const std::string& address, MediaPorts ports, bool withVideo,
                             Direction dir) {
    SessionDescription s;
    s.address = address;
    s.media.push_back({"audio", ports.audio, pts(caps.audio), dir});
    if (withVideo && caps.hasVideo()) s.media.push_back({"video", ports.video, pts(caps.video), dir});
    return s;
}

SessionDescription makeReoffer(const SessionDescription& previous, const Capabilities& caps, MediaPorts ports,
                               bool withVideo, Direction dir) {
    SessionDescription s = previous;
    s.version = previous.version + 1;
    for (auto& m : s.media) {
        m.dir = dir;
        if (m.type == "audio") {
            m.port = ports.audio;
            m.payloads = pts(caps.audio);
        }
    }
    MediaDescription* video = s.find("video");
    bool want = withVideo && caps.hasVideo();
    if (video) {
        video->port = want ? ports.video : 0;
        if (want) video->payloads = pts(caps.video);
    } else if (want) {
        s.media.push_back({"video", ports.video, pts(caps.video), dir});
    }
    return s;
}

SessionDescription makeAnswer(const SessionDescription& offer, const Capabilities& caps, const std::string& address,
                              MediaPorts ports, bool localHold) {
    SessionDescription a;
    a.address = address;
    a.sessionId = offer.sessionId + 1000;
    a.version = offer.version;
    for (const auto& m : offer.media) {
        MediaDescription r;
        r.type = m.type;
        const auto& local = capsFor(caps, m.type);
        for (int pt : m.payloads)
            if (std::find_if(local.begin(), local.end(), [pt](const Codec& c) { return c.pt == pt; }) != local.end())
                r.payloads.push_back(pt);
        if (!m.active() || r.payloads.empty()) {
            r.port = 0;
            r.payloads = {m.payloads.front()};
            r.dir = Direction::Inactive;
        } else {
            r.port = m.type == "audio" ? ports.audio : ports.video;
            r.dir = answerDirection(m.dir, localHold);
        }
        a.media.push_back(r);
    }
    return a;
}

std::vector<NegotiatedStream> negotiate(const SessionDescription& local, const SessionDescription& remote,
                                        const SessionDescription& answer) {
    std::vector<NegotiatedStream> out;
    std::size_t n = std::min({local.media.size(), remote.media.size(), answer.media.size()});
    for (std::size_t i = 0; i < n; ++i) {
        NegotiatedStream s;
        s.type = local.media[i].type;
        s.active = local.media[i].active() && remote.media[i].active();
        if (s.active) {
            s.codec = staticCodec(answer.media[i].payloads.front()).value_or(Codec{-1, "", 0});
            s.remotePort = remote.media[i].port;
            s.remoteAddress = remote.address;
            s.localDir = local.media[i].dir;
        }
        out.push_back(s);
    }
    return out;
}

}  // namespace svcc
