// SDP (RFC 4566 subset) and the offer/answer model (RFC 3264) for one audio and one video
// stream. Codecs are identified by their static RTP payload type numbers (RFC 3551) and treated
// as opaque: PCMU 0, PCMA 8, H.261 31, H.263 34.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace svcc {

struct SdpError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class Direction { SendRecv, SendOnly, RecvOnly, Inactive };

const char* toString(Direction d);
Direction answerDirection(Direction offered, bool localHold);
inline bool sends(Direction d) { return d == Direction::SendRecv || d == Direction::SendOnly; }
inline bool receives(Direction d) { return d == Direction::SendRecv || d == Direction::RecvOnly; }

struct Codec {
    int pt;
    std::string name;
    int clockRate;
    bool operator==(const Codec& o) const { return pt == o.pt; }
};

namespace codecs {
const Codec PCMU{0, "PCMU", 8000};
const Codec PCMA{8, "PCMA", 8000};
const Codec H261{31, "H261", 90000};
const Codec H263{34, "H263", 90000};
}  // namespace codecs

std::optional<Codec> staticCodec(int pt);

struct Capabilities {
    std::vector<Codec> audio;
    std::vector<Codec> video;
    bool hasVideo() const { return !video.empty(); }
};

struct MediaDescription {
    std::string type;  // "audio" or "video"
    int port = 0;      // 0 = rejected or disabled stream
    std::vector<int> payloads;
    Direction dir = Direction::SendRecv;
    bool active() const { return port != 0; }
};

struct SessionDescription {
    std::string user = "-";
    std::uint64_t sessionId = 1;
    std::uint64_t version = 1;
    std::string address;
    std::vector<MediaDescription> media;

    const MediaDescription* find(const std::string& type) const;
    MediaDescription* find(const std::string& type);
    bool hasActive(const std::string& type) const {
        auto m = find(type);
        return m && m->active();
    }
    std::string toString() const;
    static SessionDescription parse(const std::string& text);
};

struct MediaPorts {
    int audio;
    int video;
};

// An offer with audio and, if requested and supported, video.
SessionDescription makeOffer(const Capabilities& caps, const std::string& address, MediaPorts ports, bool withVideo,
                             Direction dir = Direction::SendRecv);

// A new offer for an existing session (re-INVITE): the version is incremented, m-lines are never
// removed (a dropped stream gets port 0), and a stream is added by reusing its slot or appending.
SessionDescription makeReoffer(const SessionDescription& previous, const Capabilities& caps, MediaPorts ports,
                               bool withVideo, Direction dir);

// The answer of RFC 3264 section 6: same m-lines in the same order; each accepted stream lists the
// common payload types in the offerer's preference order; unsupported streams get port 0.
SessionDescription makeAnswer(const SessionDescription& offer, const Capabilities& caps, const std::string& address,
                              MediaPorts ports, bool localHold = false);

// The outcome for one stream after an offer/answer exchange, from one side's point of view.
struct NegotiatedStream {
    std::string type;
    bool active = false;
    Codec codec{-1, "", 0};
    int remotePort = 0;
    std::string remoteAddress;
    Direction localDir = Direction::Inactive;
};

std::vector<NegotiatedStream> negotiate(const SessionDescription& local, const SessionDescription& remote,
                                        const SessionDescription& answer);

}  // namespace svcc
