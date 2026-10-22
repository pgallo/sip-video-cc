#include "svcc/media.hpp"

#include <algorithm>

namespace svcc {

namespace {
bool isVideoPt(std::uint8_t pt) { return pt == codecs::H261.pt || pt == codecs::H263.pt; }
}  // namespace

void MediaNetwork::send(const MediaAddr& from, const MediaAddr& to, std::vector<std::uint8_t> data) {
    ++sent;
    LinkProfile p;
    auto it = profiles_.find({from.host, to.host});
    if (it != profiles_.end()) p = it->second;
    bool rtcp = looksLikeRtcp(data);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    if (!rtcp && p.loss > 0 && u(rng_) < p.loss) {
        ++dropped;
        return;
    }
    Micros delay = p.delay;
    if (p.jitter > 0) delay += static_cast<Micros>(u(rng_) * static_cast<double>(p.jitter));
    if (!rtcp && data.size() > 1 && isVideoPt(data[1] & 0x7f)) delay += p.videoExtraDelay;
    sched_.schedule(delay, [this, from, to, d = std::move(data)]() {
        auto h = handlers_.find(to);
        if (h != handlers_.end()) h->second(d, from);
    });
}

// ------------------------------------------------------------------ MediaStream

MediaStream::MediaStream(Scheduler& sched, MediaNetwork& net, MediaAddr local, std::string type, std::uint32_t ssrc)
    : sched_(sched), net_(net), local_(std::move(local)), type_(std::move(type)), ssrc_(ssrc), rng_(ssrc),
      self_(std::make_shared<MediaStream*>(this)) {
    net_.bind(local_, [this](const std::vector<std::uint8_t>& d, const MediaAddr&) {
        if (running_ && receives(dir_)) onRtp(d);
    });
    net_.bind({local_.host, local_.port + 1}, [this](const std::vector<std::uint8_t>& d, const MediaAddr&) {
        if (running_) onRtcp(d);
    });
}

MediaStream::~MediaStream() {
    *self_ = nullptr;
    net_.unbind(local_);
    net_.unbind({local_.host, local_.port + 1});
}

void MediaStream::start(const NegotiatedStream& n) {
    bool restart = !running_ || !(codec_ == n.codec);
    remote_ = {n.remoteAddress, n.remotePort};
    dir_ = n.localDir;
    if (!restart) return;  // e.g. hold/resume: same RTP session, new direction
    codec_ = n.codec;
    running_ = true;
    std::uint64_t epoch = ++epoch_;
    packetizer_ = std::make_unique<Packetizer>(ssrc_, static_cast<std::uint8_t>(codec_.pt),
                                               static_cast<std::uint16_t>(rng_()));
    ts0_ = rng_();
    startedAt_ = sched_.now();
    frameNo_ = 0;
    jb_ = std::make_unique<JitterBuffer>(codec_.clockRate, video() ? msec(40) : msec(20), msec(300));
    rx_ = ReceiverStats{};
    Micros period = video() ? 66'667 : msec(20);
    auto self = self_;
    sched_.every(period, [self, epoch]() {
        if (!*self) return false;
        (*self)->sendFrame(epoch);
        return epoch == (*self)->epoch_;
    });
    sched_.every(msec(5), [self, epoch]() {
        if (!*self) return false;
        (*self)->tick(epoch);
        return epoch == (*self)->epoch_;
    });
    sched_.schedule(msec(500) + (ssrc_ % 400) * 1000, [self, epoch]() {
        if (*self) (*self)->sendRtcp(epoch);
    });
}

void MediaStream::stop() {
    running_ = false;
    ++epoch_;
}

void MediaStream::sendFrame(std::uint64_t epoch) {
    if (epoch != epoch_ || !running_ || !sends(dir_)) return;
    std::uint32_t ts;
    std::size_t bytes;
    if (video()) {
        ts = ts0_ + static_cast<std::uint32_t>((sched_.now() - startedAt_) * 90 / 1000);
        bool key = frameNo_ % 30 == 0;
        std::uniform_int_distribution<int> size(1800, 3200);
        bytes = key ? 9000 : static_cast<std::size_t>(size(rng_));
    } else {
        ts = ts0_ + 160 * frameNo_;
        bytes = 160;
    }
    ++frameNo_;
    for (auto& p : packetizer_->packetize(ts, bytes, 1200)) net_.send(local_, remote_, p.serialize());
}

void MediaStream::tick(std::uint64_t epoch) {
    if (epoch != epoch_ || !jb_) return;
    for (const auto& f : jb_->pop(sched_.now()))
        if (onPlayout) onPlayout(f.timestamp, f.playoutAt);
}

void MediaStream::sendRtcp(std::uint64_t epoch) {
    if (epoch != epoch_ || !running_) return;
    std::vector<ReportBlock> blocks;
    if (rx_.started()) {
        ReportBlock b;
        b.ssrc = remoteSsrc_;
        b.fractionLost = rx_.fractionLostSinceLastReport();
        b.cumulativeLost = rx_.cumulativeLost();
        b.extendedHighestSeq = rx_.extendedHighestSeq();
        b.jitter = rx_.jitter();
        b.lastSr = lastSrMiddle_;
        b.delaySinceLastSr = lastSrMiddle_ == 0
                                 ? 0
                                 : static_cast<std::uint32_t>((sched_.now() - lastSrArrival_) * 65536 / 1'000'000);
        blocks.push_back(b);
    }
    MediaAddr rtcpRemote{remote_.host, remote_.port + 1};
    MediaAddr rtcpLocal{local_.host, local_.port + 1};
    if (packetizer_ && packetizer_->packetsSent() > 0 && sends(dir_)) {
        SenderReport sr;
        sr.ssrc = ssrc_;
        sr.ntp = toNtp(sched_.now());
        sr.rtpTimestamp = ts0_ + static_cast<std::uint32_t>((sched_.now() - startedAt_) * codec_.clockRate / 1'000'000);
        sr.packetCount = packetizer_->packetsSent();
        sr.octetCount = packetizer_->octetsSent();
        sr.blocks = blocks;
        net_.send(rtcpLocal, rtcpRemote, serialize(sr));
    } else {
        ReceiverReport rr;
        rr.ssrc = ssrc_;
        rr.blocks = blocks;
        net_.send(rtcpLocal, rtcpRemote, serialize(rr));
    }
    auto self = self_;
    sched_.schedule(msec(1000), [self, epoch]() {
        if (*self) (*self)->sendRtcp(epoch);
    });
}

void MediaStream::onRtp(const std::vector<std::uint8_t>& data) {
    RtpPacket p;
    try {
        p = RtpPacket::parse(data);
    } catch (const RtpError&) {
        return;
    }
    if (rx_.started() && p.ssrc != remoteSsrc_) {
        // a new source (e.g. the B2BUA switched from its announcement to the agent): restart statistics
        rx_ = ReceiverStats{};
        jb_ = std::make_unique<JitterBuffer>(codec_.clockRate, video() ? msec(40) : msec(20), msec(300));
    }
    remoteSsrc_ = p.ssrc;
    auto arrival = static_cast<std::uint32_t>(sched_.now() * codec_.clockRate / 1'000'000);
    rx_.onPacket(p.seq, p.timestamp, arrival);
    jb_->push(p, sched_.now());
}

void MediaStream::onRtcp(const std::vector<std::uint8_t>& data) {
    RtcpPacket pkt;
    try {
        pkt = parseRtcp(data);
    } catch (const RtcpError&) {
        return;
    }
    const std::vector<ReportBlock>* blocks;
    if (auto* sr = std::get_if<SenderReport>(&pkt)) {
        lastSrMiddle_ = ntpMiddle32(sr->ntp);
        lastSrArrival_ = sched_.now();
        if (onSenderReport) onSenderReport(sr->ntp, sr->rtpTimestamp);
        blocks = &sr->blocks;
    } else {
        blocks = &std::get<ReceiverReport>(pkt).blocks;
    }
    for (const auto& b : *blocks) {
        if (b.ssrc != ssrc_) continue;
        remoteView_ = b;
        if (b.lastSr != 0) rttMs_ = roundTripMs(ntpMiddle32(toNtp(sched_.now())), b.lastSr, b.delaySinceLastSr);
    }
}

StreamReport MediaStream::report() const {
    StreamReport r;
    r.type = type_;
    r.codec = codec_.name;
    r.packetsSent = packetizer_ ? packetizer_->packetsSent() : 0;
    r.packetsReceived = rx_.received();
    r.cumulativeLost = rx_.cumulativeLost();
    r.jitterMs = codec_.clockRate > 0 ? rx_.jitterExact() * 1000.0 / codec_.clockRate : 0;
    r.remoteFractionLost = remoteView_.fractionLost / 256.0;
    r.remoteCumulativeLost = remoteView_.cumulativeLost;
    r.remoteJitterMs = codec_.clockRate > 0 ? remoteView_.jitter * 1000.0 / codec_.clockRate : 0;
    r.rttMs = rttMs_;
    if (jb_) {
        r.buffer = jb_->stats();
        r.playoutDelay = jb_->currentDelay() + jb_->extraDelay();
    }
    return r;
}

// ------------------------------------------------------------------ MediaSession

MediaSession::MediaSession(Scheduler& sched, MediaNetwork& net, const std::string& host, MediaPorts ports,
                           std::uint32_t ssrcBase)
    : audio_(sched, net, {host, ports.audio}, "audio", ssrcBase),
      video_(sched, net, {host, ports.video}, "video", ssrcBase + 1) {
    audio_.onPlayout = [this](std::uint32_t ts, Micros at) { lipSync_.onPlayout(false, ts, at); };
    video_.onPlayout = [this](std::uint32_t ts, Micros at) { lipSync_.onPlayout(true, ts, at); };
    audio_.onSenderReport = [this](std::uint64_t ntp, std::uint32_t ts) { lipSync_.onSenderReport(false, ntp, ts); };
    video_.onSenderReport = [this](std::uint64_t ntp, std::uint32_t ts) { lipSync_.onSenderReport(true, ntp, ts); };
}

void MediaSession::apply(const std::vector<NegotiatedStream>& streams) {
    bool audioOn = false;
    bool videoOn = false;
    for (const auto& s : streams) {
        if (!s.active) continue;
        if (s.type == "audio") {
            audio_.start(s);
            audioOn = true;
        } else if (s.type == "video") {
            video_.start(s);
            videoOn = true;
        }
    }
    if (!audioOn) audio_.stop();
    if (!videoOn) video_.stop();
}

void MediaSession::stop() {
    audio_.stop();
    video_.stop();
}

void MediaSession::correctLipSync() {
    auto off = lipSync_.offsetMs();
    if (!off || !audio_.running() || !video_.running()) return;
    Micros delta = static_cast<Micros>(*off * 1000.0);
    // positive: video is late, so hold audio back; negative: hold video back
    if (delta > 0) {
        audio_.jitterBuffer().setExtraDelay(audio_.jitterBuffer().extraDelay() + delta);
    } else {
        video_.jitterBuffer().setExtraDelay(video_.jitterBuffer().extraDelay() - delta);
    }
    correctionUs_ += delta;
}

// ------------------------------------------------------------------ MediaRelay

void MediaRelay::forward(int fromPort, MediaAddr to) {
    MediaAddr local{host_, fromPort};
    net_.bind(local, [this, local, to](const std::vector<std::uint8_t>& d, const MediaAddr&) {
        ++forwarded_;
        net_.send(local, to, d);
    });
    bound_.push_back(local);
}

void MediaRelay::connect(int localPortA, MediaAddr remoteA, int localPortB, MediaAddr remoteB) {
    for (int k = 0; k < 2; ++k) {  // RTP, then RTCP on port + 1
        forward(localPortA + k, {remoteB.host, remoteB.port + k});
        forward(localPortB + k, {remoteA.host, remoteA.port + k});
    }
}

void MediaRelay::clear() {
    for (const auto& a : bound_) net_.unbind(a);
    bound_.clear();
}

}  // namespace svcc
