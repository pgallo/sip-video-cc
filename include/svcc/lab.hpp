// Wiring for the demo and the end-to-end tests: one scheduler, the SIP and media networks, the
// contact center behind its CSTA server, a simulated commercial PBX behind its own CSTA server
// (linked to the contact center), phones and agent desktops. pump() moves CSTA bytes over the
// loopback sockets until all parties are quiet, so virtual time never advances while a request
// is in flight.
#pragma once

#include "svcc/contact_center.hpp"
#include "svcc/csta_net.hpp"
#include "svcc/desktop.hpp"
#include "svcc/pbx.hpp"
#include "svcc/phone.hpp"

#include <memory>
#include <vector>

namespace svcc {

inline Capabilities videoPhoneCaps() { return {{codecs::PCMU, codecs::PCMA}, {codecs::H263, codecs::H261}}; }
inline Capabilities audioPhoneCaps() { return {{codecs::PCMA}, {}}; }

struct Lab {
    explicit Lab(std::uint32_t seed = 7, Micros videoFallbackWait = msec(5000))
        : sip(sched, msec(2)), net(sched, seed), cc(sched, sip, net, "cc", videoFallbackWait), ccServer(cc),
          pbx(sched, sip, net, "pbx"), pbxServer(pbx) {
        sip.attach("cc", &cc);
        sip.attach("pbx", &pbx);
        cc.setEventSink([this](const CstaMessage& e) { ccServer.publish(e); });
        pbx.setEventSink([this](const CstaMessage& e) { pbxServer.publish(e); });
    }

    void connectPbx() {
        pbxLink = std::make_unique<CstaClient>(pbxServer.port());
        pbxLink->onEvent = [this](const CstaMessage& e) { cc.onPbxEvent(e); };
        pump();
        cc.attachPbx(pbxLink.get());
        pump();
    }

    SipPhone& phone(const std::string& address, const std::string& user, Capabilities caps) {
        phones.push_back(std::make_unique<SipPhone>(sched, sip, net, address, user, std::move(caps),
                                                    10000 + 400 * static_cast<int>(phones.size())));
        sip.attach(address, phones.back().get());
        return *phones.back();
    }

    AgentDesktop& desktop(const std::string& device) {
        desktops.push_back(std::make_unique<AgentDesktop>(sched, ccServer.port(), device));
        pump();
        return *desktops.back();
    }

    int pump() {
        int total = 0;
        for (int quiet = 0; quiet < 2;) {
            int n = ccServer.poll(0) + pbxServer.poll(0);
            if (pbxLink) n += pbxLink->poll(0);
            for (auto& d : desktops) n += d->poll(0);
            for (auto* c : extraClients) n += c->poll(0);
            total += n;
            quiet = n == 0 ? quiet + 1 : 0;
        }
        return total;
    }

    void runUntil(Micros t, Micros step = msec(5)) {
        while (sched.now() < t) {
            sched.runUntil(std::min(t, sched.now() + step));
            pump();
        }
    }

    Scheduler sched;
    SipNetwork sip;
    MediaNetwork net;
    ContactCenter cc;
    CstaServer ccServer;
    SimulatedPbx pbx;
    CstaServer pbxServer;
    std::unique_ptr<CstaClient> pbxLink;
    std::vector<std::unique_ptr<SipPhone>> phones;
    std::vector<std::unique_ptr<AgentDesktop>> desktops;
    std::vector<CstaClient*> extraClients;
};

}  // namespace svcc
