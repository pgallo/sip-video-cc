// A scripted agent desktop: a CSTA client that monitors one agent device, goes ready, answers
// delivered calls with AnswerCall after a short delay, and after a talk time either transfers
// with SingleStepTransferCall or clears with ClearConnection. Its timers run on the simulation
// scheduler; its requests go over TCP like any other CSTA application.
#pragma once

#include "svcc/csta_net.hpp"
#include "svcc/sim.hpp"

#include <deque>
#include <string>
#include <vector>

namespace svcc {

class AgentDesktop {
public:
    AgentDesktop(Scheduler& sched, std::uint16_t port, std::string device);

    void start(bool ready = true);
    int poll(int timeoutMs) { return client_.poll(timeoutMs); }
    void setReady(bool ready);

    Micros answerDelay = msec(1500);
    Micros talkTime = msec(8000);
    std::deque<std::string> transfers;  // transfer targets, used in order
    bool autoAnswer = true;
    std::vector<std::string> events;    // time-stamped event log
    std::vector<std::string> errors;

private:
    void onEvent(const CstaMessage& m);
    void checkResponse(const CstaMessage& m);

    Scheduler& sched_;
    CstaClient client_;
    std::string device_;
};

}  // namespace svcc
