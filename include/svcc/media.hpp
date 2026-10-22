// Media plane: a simulated packet network with per-path delay, jitter and loss; RTP streams that
// send synthetic audio and video frames, receive through a jitter buffer and exchange RTCP; a
// per-call session that ties audio and video together for lip sync; and the RTP relay a B2BUA
// uses to anchor media between two call legs.
#pragma once

#include "svcc/rtcp.hpp"
#include "svcc/rtp.hpp"
#include "svcc/sdp.hpp"
#include "svcc/sim.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace svcc {

struct MediaAddr {
    std::string host;
    int port = 0;
    bool operator<(const MediaAddr& o) const { return host != o.host ? host < o.host : port < o.port; }
    bool operator==(const MediaAddr& o) const { return host == o.host && port == o.port; }
};

struct LinkProfile {
    Micros delay = msec(5);
    Micros jitter = 0;          // uniform extra delay in [0, jitter]
    double loss = 0;            // RTP only; RTCP is not dropped
    Micros videoExtraDelay = 0;  // extra delay for video RTP (models a slower path for large packets)
};

class MediaNetwork {
public:
    using Handler = std::function<void(const std::vector<std::uint8_t>&, const MediaAddr& from)>;

    MediaNetwork(Scheduler& sched, std::uint32_t seed) : sched_(sched), rng_(seed) {}
    void bind(const MediaAddr& a, Handler h) { handlers_[a] = std::move(h); }
    void unbind(const MediaAddr& a) { handlers_.erase(a); }
    void setProfile(const std::string& fromHost, const std::string& toHost, LinkProfile p) {
        profiles_[{fromHost, toHost}] = p;
    }
    void send(const MediaAddr& from, const MediaAddr& to, std::vector<std::uint8_t> data);

    std::uint64_t sent = 0;
    std::uint64_t dropped = 0;

private:
    Scheduler& sched_;
    std::mt19937 rng_;
    std::map<MediaAddr, Handler> handlers_;
    std::map<std::pair<std::string, std::string>, LinkProfile> profiles_;
};

struct StreamReport {
    std::string type;
    std::string codec;
    std::uint32_t packetsSent = 0;
    std::uint32_t packetsReceived = 0;
    std::int32_t cumulativeLost = 0;
    double jitterMs = 0;            // our own measurement of the incoming stream
    double remoteFractionLost = 0;  // what the far end reports about our outgoing stream
    std::int32_t remoteCumulativeLost = 0;
    double remoteJitterMs = 0;
    double rttMs = 0;
    JitterStats buffer;
    Micros playoutDelay = 0;
};

// One RTP stream (audio or video) with its RTCP companion on port + 1.
class MediaStream {
public:
    MediaStream(Scheduler& sched, MediaNetwork& net, MediaAddr local, std::string type, std::uint32_t ssrc);
    ~MediaStream();
    MediaStream(const MediaStream&) = delete;
    MediaStream& operator=(const MediaStream&) = delete;

    void start(const NegotiatedStream& n);
    void stop();
    bool running() const { return running_; }
    bool video() const { return type_ == "video"; }
    StreamReport report() const;
    JitterBuffer& jitterBuffer() { return *jb_; }

    std::function<void(std::uint32_t ts, Micros playedAt)> onPlayout;
    std::function<void(std::uint64_t ntp, std::uint32_t rtpTs)> onSenderReport;

private:
    void sendFrame(std::uint64_t epoch);
    void tick(std::uint64_t epoch);
    void sendRtcp(std::uint64_t epoch);
    void onRtp(const std::vector<std::uint8_t>& data);
    void onRtcp(const std::vector<std::uint8_t>& data);

    Scheduler& sched_;
    MediaNetwork& net_;
    MediaAddr local_;
    MediaAddr remote_;
    std::string type_;
    std::uint32_t ssrc_;
    Codec codec_{-1, "", 0};
    Direction dir_ = Direction::Inactive;
    bool running_ = false;
    std::uint64_t epoch_ = 0;
    std::unique_ptr<Packetizer> packetizer_;
    std::uint32_t ts0_ = 0;
    Micros startedAt_ = 0;
    std::uint32_t frameNo_ = 0;
    std::unique_ptr<JitterBuffer> jb_;
    ReceiverStats rx_;
    std::uint32_t remoteSsrc_ = 0;
    std::uint32_t lastSrMiddle_ = 0;
    Micros lastSrArrival_ = 0;
    ReportBlock remoteView_;
    double rttMs_ = 0;
    std::mt19937 rng_;
    // Scheduled tasks hold this token, so a task that fires after the stream is destroyed is a no-op.
    std::shared_ptr<MediaStream*> self_;
};

// The media of one call on one endpoint: an audio and an optional video stream, plus lip sync.
class MediaSession {
public:
    MediaSession(Scheduler& sched, MediaNetwork& net, const std::string& host, MediaPorts ports, std::uint32_t ssrcBase);

    // Applies the result of an offer/answer exchange: starts, restarts or stops each stream.
    void apply(const std::vector<NegotiatedStream>& streams);
    void stop();

    // Holds back the earlier stream so audio and video are presented together.
    void correctLipSync();
    std::optional<double> lipSyncOffsetMs() const { return lipSync_.offsetMs(); }
    double lipSyncCorrectionMs() const { return correctionUs_ / 1000.0; }

    MediaStream& audio() { return audio_; }
    MediaStream& video() { return video_; }

private:
    MediaStream audio_;
    MediaStream video_;
    LipSync lipSync_{8000, 90000};
    Micros correctionUs_ = 0;
};

// Anchors media between two legs: everything arriving on a leg's local ports (RTP and RTCP) is
// forwarded unchanged to the other leg's remote address, so SSRCs and SR mappings stay end to end.
class MediaRelay {
public:
    MediaRelay(MediaNetwork& net, std::string host) : net_(net), host_(std::move(host)) {}
    ~MediaRelay() { clear(); }
    MediaRelay(const MediaRelay&) = delete;
    MediaRelay& operator=(const MediaRelay&) = delete;

    void connect(int localPortA, MediaAddr remoteA, int localPortB, MediaAddr remoteB);
    void clear();
    std::uint64_t forwarded() const { return forwarded_; }

private:
    void forward(int fromPort, MediaAddr to);
    MediaNetwork& net_;
    std::string host_;
    std::vector<MediaAddr> bound_;
    std::uint64_t forwarded_ = 0;
};

}  // namespace svcc
