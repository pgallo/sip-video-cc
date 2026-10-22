// CSTA services (ECMA-269) encoded as ECMA-323 XML: the call-control and agent services a
// contact center needs, their positive responses, the error response and the events used here.
// Also the TCP framing used by the server and client in csta_net.hpp.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>

namespace svcc {

struct CstaDecodeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline const char* kCstaNamespace = "http://www.ecma-international.org/standards/ecma-323/csta/ed3";

struct ConnectionId {
    std::string callId;
    std::string deviceId;
    bool operator==(const ConnectionId& o) const { return callId == o.callId && deviceId == o.deviceId; }
};

// requests
struct MonitorStart { std::string device; };
struct MakeCall { std::string callingDevice; std::string calledDirectoryNumber; };
struct AnswerCall { ConnectionId callToBeAnswered; };
struct ClearConnection { ConnectionId connectionToBeCleared; };
struct SingleStepTransferCall { ConnectionId activeCall; std::string transferredTo; };
struct SetAgentState { std::string device; std::string requestedAgentState; };  // loggedOn, loggedOff, ready, notReady

// responses
struct MonitorStartResponse { std::string crossRefId; };
struct MakeCallResponse { ConnectionId callingDevice; };
struct AnswerCallResponse {};
struct ClearConnectionResponse {};
struct SingleStepTransferCallResponse { ConnectionId transferredCall; };
struct SetAgentStateResponse {};
struct CstaErrorCode { std::string operation; };  // e.g. invalidCallID, invalidDeviceID, invalidObjectState

// events
struct DeliveredEvent {
    std::string crossRefId;
    ConnectionId connection;
    std::string alertingDevice;
    std::string callingDevice;
    std::string calledDevice;
    bool video = false;
    std::string correlatorData;
    std::string cause = "newCall";
};
struct EstablishedEvent {
    std::string crossRefId;
    ConnectionId establishedConnection;
    std::string answeringDevice;
    std::string callingDevice;
    std::string calledDevice;
    bool video = false;
    std::string cause = "normal";
};
struct ConnectionClearedEvent {
    std::string crossRefId;
    ConnectionId droppedConnection;
    std::string releasingDevice;
    std::string cause = "normalClearing";
};
struct AgentReadyEvent { std::string crossRefId; std::string agentDevice; };
struct AgentNotReadyEvent { std::string crossRefId; std::string agentDevice; };

using CstaMessage =
    std::variant<MonitorStart, MakeCall, AnswerCall, ClearConnection, SingleStepTransferCall, SetAgentState,
                 MonitorStartResponse, MakeCallResponse, AnswerCallResponse, ClearConnectionResponse,
                 SingleStepTransferCallResponse, SetAgentStateResponse, CstaErrorCode, DeliveredEvent,
                 EstablishedEvent, ConnectionClearedEvent, AgentReadyEvent, AgentNotReadyEvent>;

std::string encodeXml(const CstaMessage& m);
CstaMessage decodeXml(const std::string& xml);
std::string messageName(const CstaMessage& m);
bool isEvent(const CstaMessage& m);
// The device an event is about, used to route it to monitors of that device.
std::string eventDevice(const CstaMessage& m);
void setCrossRef(CstaMessage& m, const std::string& crossRefId);
std::string describe(const CstaMessage& m);

// Framing on TCP: 2 bytes 0x0000, 2-byte big-endian total length (header included), 4 ASCII
// digits of invoke id, then the XML. Events use invoke id 9999.
constexpr int kEventInvokeId = 9999;
std::string frame(int invokeId, const std::string& xml);

class FrameDecoder {
public:
    void feed(const char* data, std::size_t n) { buf_.append(data, n); }
    // Extracts the next complete frame; throws CstaDecodeError on a malformed header.
    bool next(int& invokeId, std::string& xml);

private:
    std::string buf_;
};

}  // namespace svcc
