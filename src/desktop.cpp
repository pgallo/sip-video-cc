#include "svcc/desktop.hpp"

#include <cstdio>

namespace svcc {

AgentDesktop::AgentDesktop(Scheduler& sched, std::uint16_t port, std::string device)
    : sched_(sched), client_(port), device_(std::move(device)) {
    client_.onEvent = [this](const CstaMessage& m) { onEvent(m); };
}

void AgentDesktop::start(bool ready) {
    client_.request(MonitorStart{device_}, [this](const CstaMessage& m) { checkResponse(m); });
    client_.request(SetAgentState{device_, "loggedOn"}, [this](const CstaMessage& m) { checkResponse(m); });
    if (ready) setReady(true);
}

void AgentDesktop::setReady(bool ready) {
    client_.request(SetAgentState{device_, ready ? "ready" : "notReady"},
                    [this](const CstaMessage& m) { checkResponse(m); });
}

void AgentDesktop::checkResponse(const CstaMessage& m) {
    if (auto* e = std::get_if<CstaErrorCode>(&m)) errors.push_back(e->operation);
}

void AgentDesktop::onEvent(const CstaMessage& m) {
    char t[32];
    std::snprintf(t, sizeof t, "[%8.3fs] ", static_cast<double>(sched_.now()) / 1e6);
    events.push_back(t + device_ + " <- " + describe(m));
    if (auto* d = std::get_if<DeliveredEvent>(&m)) {
        if (!autoAnswer) return;
        ConnectionId conn = d->connection;
        sched_.schedule(answerDelay, [this, conn]() {
            client_.request(AnswerCall{conn}, [this](const CstaMessage& r) { checkResponse(r); });
        });
    } else if (auto* e = std::get_if<EstablishedEvent>(&m)) {
        if (e->cause == "makeCall") return;  // calls placed by a CTI application are cleared by it
        ConnectionId conn = e->establishedConnection;
        std::string target;
        if (!transfers.empty()) {
            target = transfers.front();
            transfers.pop_front();
        }
        sched_.schedule(talkTime, [this, conn, target]() {
            if (target.empty())
                client_.request(ClearConnection{conn}, [this](const CstaMessage& r) { checkResponse(r); });
            else
                client_.request(SingleStepTransferCall{conn, target}, [this](const CstaMessage& r) { checkResponse(r); });
        });
    }
}

}  // namespace svcc
