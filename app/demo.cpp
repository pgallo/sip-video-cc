// Demo: a video caller is routed to a video-capable agent, answered through CSTA, put on hold and
// resumed, lip-synced, then transferred with SingleStepTransferCall to an audio-only agent (the
// caller's video is dropped with a re-INVITE). A second video caller falls back to an audio
// agent whose desk phone lives on a simulated commercial PBX, controlled through that PBX's CSTA
// interface. An audio caller and a MakeCall (third-party call control) complete the run.
// Prints RTCP statistics, the CSTA event log and call records; exits non-zero if a check fails.

#include "svcc/lab.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

using namespace svcc;

namespace {

std::string fmtStream(const std::string& who, const StreamReport& r) {
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  "  %-10s %-5s %-4s sent %4u recv %4u lost %2d jitter %4.1f ms | far end: lost %2d jitter %4.1f ms | "
                  "rtt %4.1f ms | buffer: late %2llu concealed %2llu incomplete %2llu delay %3lld ms",
                  who.c_str(), r.type.c_str(), r.codec.c_str(), r.packetsSent, r.packetsReceived, r.cumulativeLost,
                  r.jitterMs, r.remoteCumulativeLost, r.remoteJitterMs, r.rttMs,
                  static_cast<unsigned long long>(r.buffer.late), static_cast<unsigned long long>(r.buffer.packetsConcealed),
                  static_cast<unsigned long long>(r.buffer.framesIncomplete), static_cast<long long>(r.playoutDelay / 1000));
    return buf;
}

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::printf("CHECK FAILED: %s\n", what.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool quiet = argc > 1 && std::strcmp(argv[1], "--quiet") == 0;
    Lab lab(7);

    // network: callers come in over a lossy, jittery access link where video takes a slower path
    for (const char* caller : {"caller-1", "caller-2", "caller-3"}) {
        lab.net.setProfile(caller, "cc", {msec(20), msec(30), 0.01, msec(60)});
        lab.net.setProfile("cc", caller, {msec(20), msec(30), 0.01, msec(60)});
    }
    for (const char* agent : {"ana-pc", "ben-phone", "pbx", "customer-4"}) {
        lab.net.setProfile(agent, "cc", {msec(4), msec(4), 0, 0});
        lab.net.setProfile("cc", agent, {msec(4), msec(4), 0, 0});
    }

    lab.cc.addAgent({"2001", "Ana", "ana-pc", true, false});
    lab.cc.addAgent({"2002", "Ben", "ben-phone", false, false});
    lab.cc.addAgent({"3001", "Carla", "pbx", false, true});
    lab.cc.addDirectoryNumber("5550100", "customer-4");
    lab.pbx.addDevice("3001");
    lab.connectPbx();

    SipPhone& ana = lab.phone("ana-pc", "2001", videoPhoneCaps());
    lab.phone("ben-phone", "2002", audioPhoneCaps());
    SipPhone& caller1 = lab.phone("caller-1", "caller1", videoPhoneCaps());
    SipPhone& caller2 = lab.phone("caller-2", "caller2", videoPhoneCaps());
    SipPhone& caller3 = lab.phone("caller-3", "caller3", audioPhoneCaps());
    SipPhone& customer4 = lab.phone("customer-4", "5550100", videoPhoneCaps());
    customer4.autoAnswer = true;

    AgentDesktop& dAna = lab.desktop("2001");
    AgentDesktop& dBen = lab.desktop("2002");
    AgentDesktop& dCarla = lab.desktop("3001");
    dAna.talkTime = msec(11000);
    dAna.transfers.push_back("2002");  // C1 goes from Ana (video) to Ben (audio only)
    dBen.talkTime = msec(8000);
    dCarla.talkTime = msec(12000);
    dAna.start(true);
    dBen.start(false);    // Ben becomes ready at t=10 s
    dCarla.start(false);  // Carla's readiness comes from the PBX
    lab.pump();

    CstaClient supervisor(lab.ccServer.port());  // a CTI application placing an outbound call
    lab.extraClients.push_back(&supervisor);
    lab.pump();

    lab.sched.schedule(msec(500), [&] { lab.pbx.agentReady("3001", true); });
    lab.sched.schedule(msec(10000), [&] { dBen.setReady(true); });

    std::string c1, c2, c3;
    lab.sched.schedule(msec(1000), [&] { c1 = caller1.call("sip:support@cc", "cc", true); });
    lab.sched.schedule(msec(3000), [&] { c2 = caller2.call("sip:support@cc", "cc", true); });
    lab.sched.schedule(msec(16000), [&] { c3 = caller3.call("sip:support@cc", "cc", false); });

    // Ana holds and resumes, then corrects lip sync once enough SRs have arrived
    lab.sched.schedule(msec(6500), [&] {
        for (const auto& id : ana.callIds()) ana.reinvite(id, true, true);
    });
    lab.sched.schedule(msec(7500), [&] {
        for (const auto& id : ana.callIds()) ana.reinvite(id, true, false);
    });
    std::optional<double> offsetBefore, offsetAfter;
    double correction = 0;
    lab.sched.schedule(msec(9000), [&] {
        for (const auto& id : ana.callIds()) {
            const SipPhone::Call* call = ana.find(id);
            offsetBefore = call->media->lipSyncOffsetMs();
            call->media->correctLipSync();
            correction = call->media->lipSyncCorrectionMs();
        }
    });
    std::vector<std::string> rtcpLines;
    bool c1VideoWithAna = false;
    lab.sched.schedule(msec(12500), [&] {
        for (const auto& id : ana.callIds()) {
            const SipPhone::Call* call = ana.find(id);
            offsetAfter = call->media->lipSyncOffsetMs();
            c1VideoWithAna = ana.videoActive(id);
            rtcpLines.push_back(fmtStream("agent 2001", call->media->audio().report()));
            rtcpLines.push_back(fmtStream("agent 2001", call->media->video().report()));
        }
        if (const SipPhone::Call* call = caller1.find(c1)) {
            rtcpLines.push_back(fmtStream("caller-1", call->media->audio().report()));
            rtcpLines.push_back(fmtStream("caller-1", call->media->video().report()));
        }
    });
    bool c1VideoAfterTransfer = true;
    lab.sched.schedule(msec(19000), [&] { c1VideoAfterTransfer = caller1.videoActive(c1); });

    lab.sched.schedule(msec(34000), [&] {
        supervisor.request(MakeCall{"2001", "5550100"}, [&](const CstaMessage& r) {
            if (auto* ok = std::get_if<MakeCallResponse>(&r)) {
                ConnectionId conn = ok->callingDevice;
                lab.sched.schedule(msec(5000), [&supervisor, conn] { supervisor.request(ClearConnection{conn}); });
            }
        });
    });

    lab.runUntil(msec(45000));
    lab.sched.runUntil(lab.sched.now() + msec(1000));
    lab.pump();

    // ---------------------------------------------------------------- report
    if (!quiet) {
        std::printf("== contact center (B2BUA, routing, CSTA-to-SIP mapping) ==\n");
        for (const auto& l : lab.cc.log) std::printf("%s\n", l.c_str());

        std::printf("\n== CSTA events received by agent desktops (ECMA-323 over TCP) ==\n");
        std::multimap<std::string, std::string> merged;
        for (auto* d : {&dAna, &dBen, &dCarla})
            for (const auto& e : d->events) merged.emplace(e.substr(0, 12), e);
        for (const auto& kv : merged) std::printf("%s\n", kv.second.c_str());

        std::printf("\n== RTCP and jitter buffer, call C1 at t=12.5 s (before the transfer) ==\n");
        for (const auto& l : rtcpLines) std::printf("%s\n", l.c_str());
        std::printf("\n== lip sync at agent 2001 ==\n");
        std::printf("  A/V offset before correction: %+.1f ms (video later than audio)\n", offsetBefore.value_or(NAN));
        std::printf("  audio held back by:           %.1f ms\n", correction);
        std::printf("  A/V offset after correction:  %+.1f ms\n", offsetAfter.value_or(NAN));

        std::printf("\n== call records ==\n");
        std::printf("  call  from      dir  video offered/at answer  agents     transfers  video re-INVITEs  answered  outcome\n");
        for (const auto& r : lab.cc.records()) {
            std::string agents;
            for (const auto& a : r.agents) agents += (agents.empty() ? "" : ",") + a;
            std::string video = std::string(r.outbound ? "-" : r.callerOfferedVideo ? "yes" : "no") + " / " +
                                (r.videoAtFirstAnswer ? "yes" : "no");
            std::printf("  %-5s %-9s %-4s %-24s %-10s %9d  %16d  %7.1fs  %s\n", r.callId.c_str(), r.caller.c_str(),
                        r.outbound ? "out" : "in", video.c_str(), agents.c_str(), r.transfers,
                        r.videoReinvites, r.answered < 0 ? -1.0 : r.answered / 1e6, r.outcome.c_str());
        }
        std::printf("\nSIP messages delivered: %llu, media packets sent: %llu (dropped by the network: %llu), "
                    "relayed by the B2BUA: %llu\n",
                    static_cast<unsigned long long>(lab.sip.delivered), static_cast<unsigned long long>(lab.net.sent),
                    static_cast<unsigned long long>(lab.net.dropped),
                    static_cast<unsigned long long>(lab.cc.relayedPackets()));
    }

    // ---------------------------------------------------------------- checks
    const auto& recs = lab.cc.records();
    check(recs.size() == 4, "four calls recorded");
    if (recs.size() == 4) {
        check(recs[0].agents == std::vector<std::string>{"2001", "2002"}, "C1 handled by 2001 then 2002");
        check(recs[0].videoAtFirstAnswer, "C1 answered with video");
        check(recs[0].transfers == 1 && recs[0].videoReinvites == 1, "C1 transferred once, video dropped by re-INVITE");
        check(recs[1].agents == std::vector<std::string>{"3001"}, "C2 fell back to the PBX audio agent");
        check(recs[2].agents == std::vector<std::string>{"2001"}, "C3 answered by 2001");
        check(recs[3].outbound && recs[3].answered >= 0, "MakeCall connected");
        for (const auto& r : recs) check(r.outcome == "completed", r.callId + " completed");
    }
    check(c1VideoWithAna, "video active between caller-1 and Ana");
    check(!c1VideoAfterTransfer, "caller-1 video dropped after transfer to an audio agent");
    check(offsetBefore && *offsetBefore > 20, "lip-sync offset measured (video late)");
    check(offsetAfter && std::fabs(*offsetAfter) < 15, "lip-sync offset corrected");
    check(lab.cc.activeCalls() == 0, "no calls left");
    for (auto* d : {&dAna, &dBen, &dCarla})
        for (const auto& e : d->errors) check(false, "desktop request rejected: " + e);
    check(lab.net.dropped > 0, "the lossy access link dropped packets");
    if (!quiet) std::printf("\n%s\n", failures == 0 ? "all demo checks passed" : "demo checks FAILED");
    return failures == 0 ? 0 : 1;
}
