#include "svcc/csta.hpp"

#include "svcc/xml.hpp"

#include <cstdio>

namespace svcc {

namespace {

template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

void putConn(XmlNode& parent, const std::string& name, const ConnectionId& c) {
    XmlNode& n = parent.add(name);
    n.add("callID", c.callId);
    n.add("deviceID", c.deviceId);
}

ConnectionId getConn(const XmlNode& root, const std::string& name) {
    const XmlNode* n = root.child(name);
    if (!n) throw CstaDecodeError("missing " + name);
    ConnectionId c{n->path("callID"), n->path("deviceID")};
    if (c.callId.empty() && c.deviceId.empty()) throw CstaDecodeError("empty connection " + name);
    return c;
}

void putDevice(XmlNode& parent, const std::string& name, const std::string& id) {
    parent.add(name).add("deviceIdentifier", id);
}

std::string required(const XmlNode& root, const std::string& path) {
    std::string v = root.path(path);
    if (v.empty()) throw CstaDecodeError("missing " + path);
    return v;
}

void putMedia(XmlNode& parent, bool video) {
    XmlNode& mc = parent.add("mediaCallCharacteristics").add("mediaClass");
    mc.add("voice", "true");
    mc.add("video", video ? "true" : "false");
}

XmlNode root(const std::string& name) {
    XmlNode n;
    n.name = name;
    n.attributes.emplace_back("xmlns", kCstaNamespace);
    return n;
}

}  // namespace

std::string encodeXml(const CstaMessage& m) {
    XmlNode x = std::visit(
        overloaded{
            [](const MonitorStart& v) {
                XmlNode n = root("MonitorStart");
                n.add("monitorObject").add("deviceObject", v.device);
                return n;
            },
            [](const MakeCall& v) {
                XmlNode n = root("MakeCall");
                n.add("callingDevice", v.callingDevice);
                n.add("calledDirectoryNumber", v.calledDirectoryNumber);
                return n;
            },
            [](const AnswerCall& v) {
                XmlNode n = root("AnswerCall");
                putConn(n, "callToBeAnswered", v.callToBeAnswered);
                return n;
            },
            [](const ClearConnection& v) {
                XmlNode n = root("ClearConnection");
                putConn(n, "connectionToBeCleared", v.connectionToBeCleared);
                return n;
            },
            [](const SingleStepTransferCall& v) {
                XmlNode n = root("SingleStepTransferCall");
                putConn(n, "activeCall", v.activeCall);
                n.add("transferredTo", v.transferredTo);
                return n;
            },
            [](const SetAgentState& v) {
                XmlNode n = root("SetAgentState");
                n.add("device", v.device);
                n.add("requestedAgentState", v.requestedAgentState);
                return n;
            },
            [](const MonitorStartResponse& v) {
                XmlNode n = root("MonitorStartResponse");
                n.add("monitorCrossRefID", v.crossRefId);
                return n;
            },
            [](const MakeCallResponse& v) {
                XmlNode n = root("MakeCallResponse");
                putConn(n, "callingDevice", v.callingDevice);
                return n;
            },
            [](const AnswerCallResponse&) { return root("AnswerCallResponse"); },
            [](const ClearConnectionResponse&) { return root("ClearConnectionResponse"); },
            [](const SingleStepTransferCallResponse& v) {
                XmlNode n = root("SingleStepTransferCallResponse");
                putConn(n, "transferredCall", v.transferredCall);
                return n;
            },
            [](const SetAgentStateResponse&) { return root("SetAgentStateResponse"); },
            [](const CstaErrorCode& v) {
                XmlNode n = root("CSTAErrorCode");
                n.add("operation", v.operation);
                return n;
            },
            [](const DeliveredEvent& v) {
                XmlNode n = root("DeliveredEvent");
                n.add("monitorCrossRefID", v.crossRefId);
                putConn(n, "connection", v.connection);
                putDevice(n, "alertingDevice", v.alertingDevice);
                putDevice(n, "callingDevice", v.callingDevice);
                putDevice(n, "calledDevice", v.calledDevice);
                n.add("lastRedirectionDevice").add("notRequired");
                n.add("localConnectionInfo", "alerting");
                if (!v.correlatorData.empty()) n.add("correlatorData").add("string", v.correlatorData);
                n.add("cause", v.cause);
                putMedia(n, v.video);
                return n;
            },
            [](const EstablishedEvent& v) {
                XmlNode n = root("EstablishedEvent");
                n.add("monitorCrossRefID", v.crossRefId);
                putConn(n, "establishedConnection", v.establishedConnection);
                putDevice(n, "answeringDevice", v.answeringDevice);
                putDevice(n, "callingDevice", v.callingDevice);
                putDevice(n, "calledDevice", v.calledDevice);
                n.add("lastRedirectionDevice").add("notRequired");
                n.add("localConnectionInfo", "connected");
                n.add("cause", v.cause);
                putMedia(n, v.video);
                return n;
            },
            [](const ConnectionClearedEvent& v) {
                XmlNode n = root("ConnectionClearedEvent");
                n.add("monitorCrossRefID", v.crossRefId);
                putConn(n, "droppedConnection", v.droppedConnection);
                putDevice(n, "releasingDevice", v.releasingDevice);
                n.add("localConnectionInfo", "null");
                n.add("cause", v.cause);
                return n;
            },
            [](const AgentReadyEvent& v) {
                XmlNode n = root("AgentReadyEvent");
                n.add("monitorCrossRefID", v.crossRefId);
                putDevice(n, "agentDevice", v.agentDevice);
                return n;
            },
            [](const AgentNotReadyEvent& v) {
                XmlNode n = root("AgentNotReadyEvent");
                n.add("monitorCrossRefID", v.crossRefId);
                putDevice(n, "agentDevice", v.agentDevice);
                return n;
            },
        },
        m);
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" + x.toString();
}

CstaMessage decodeXml(const std::string& xml) {
    XmlNode x;
    try {
        x = parseXml(xml);
    } catch (const XmlError& e) {
        throw CstaDecodeError(std::string("bad XML: ") + e.what());
    }
    const std::string& n = x.name;
    bool video = x.path("mediaCallCharacteristics/mediaClass/video") == "true";
    if (n == "MonitorStart") return MonitorStart{required(x, "monitorObject/deviceObject")};
    if (n == "MakeCall") return MakeCall{required(x, "callingDevice"), required(x, "calledDirectoryNumber")};
    if (n == "AnswerCall") return AnswerCall{getConn(x, "callToBeAnswered")};
    if (n == "ClearConnection") return ClearConnection{getConn(x, "connectionToBeCleared")};
    if (n == "SingleStepTransferCall")
        return SingleStepTransferCall{getConn(x, "activeCall"), required(x, "transferredTo")};
    if (n == "SetAgentState") {
        std::string state = required(x, "requestedAgentState");
        if (state != "loggedOn" && state != "loggedOff" && state != "ready" && state != "notReady")
            throw CstaDecodeError("unknown agent state " + state);
        return SetAgentState{required(x, "device"), state};
    }
    if (n == "MonitorStartResponse") return MonitorStartResponse{required(x, "monitorCrossRefID")};
    if (n == "MakeCallResponse") return MakeCallResponse{getConn(x, "callingDevice")};
    if (n == "AnswerCallResponse") return AnswerCallResponse{};
    if (n == "ClearConnectionResponse") return ClearConnectionResponse{};
    if (n == "SingleStepTransferCallResponse") return SingleStepTransferCallResponse{getConn(x, "transferredCall")};
    if (n == "SetAgentStateResponse") return SetAgentStateResponse{};
    if (n == "CSTAErrorCode") return CstaErrorCode{required(x, "operation")};
    if (n == "DeliveredEvent") {
        DeliveredEvent e;
        e.crossRefId = x.path("monitorCrossRefID");
        e.connection = getConn(x, "connection");
        e.alertingDevice = x.path("alertingDevice/deviceIdentifier");
        e.callingDevice = x.path("callingDevice/deviceIdentifier");
        e.calledDevice = x.path("calledDevice/deviceIdentifier");
        e.correlatorData = x.path("correlatorData/string");
        e.cause = x.path("cause");
        e.video = video;
        return e;
    }
    if (n == "EstablishedEvent") {
        EstablishedEvent e;
        e.crossRefId = x.path("monitorCrossRefID");
        e.establishedConnection = getConn(x, "establishedConnection");
        e.answeringDevice = x.path("answeringDevice/deviceIdentifier");
        e.callingDevice = x.path("callingDevice/deviceIdentifier");
        e.calledDevice = x.path("calledDevice/deviceIdentifier");
        e.cause = x.path("cause");
        e.video = video;
        return e;
    }
    if (n == "ConnectionClearedEvent") {
        ConnectionClearedEvent e;
        e.crossRefId = x.path("monitorCrossRefID");
        e.droppedConnection = getConn(x, "droppedConnection");
        e.releasingDevice = x.path("releasingDevice/deviceIdentifier");
        e.cause = x.path("cause");
        return e;
    }
    if (n == "AgentReadyEvent") return AgentReadyEvent{x.path("monitorCrossRefID"), x.path("agentDevice/deviceIdentifier")};
    if (n == "AgentNotReadyEvent")
        return AgentNotReadyEvent{x.path("monitorCrossRefID"), x.path("agentDevice/deviceIdentifier")};
    throw CstaDecodeError("unsupported CSTA message " + n);
}

std::string messageName(const CstaMessage& m) {
    static const char* names[] = {"MonitorStart", "MakeCall", "AnswerCall", "ClearConnection", "SingleStepTransferCall",
                                  "SetAgentState", "MonitorStartResponse", "MakeCallResponse", "AnswerCallResponse",
                                  "ClearConnectionResponse", "SingleStepTransferCallResponse", "SetAgentStateResponse",
                                  "CSTAErrorCode", "DeliveredEvent", "EstablishedEvent", "ConnectionClearedEvent",
                                  "AgentReadyEvent", "AgentNotReadyEvent"};
    return names[m.index()];
}

bool isEvent(const CstaMessage& m) { return m.index() >= 13; }

std::string eventDevice(const CstaMessage& m) {
    return std::visit(overloaded{[](const DeliveredEvent& e) { return e.alertingDevice; },
                                 [](const EstablishedEvent& e) { return e.answeringDevice; },
                                 [](const ConnectionClearedEvent& e) { return e.droppedConnection.deviceId; },
                                 [](const AgentReadyEvent& e) { return e.agentDevice; },
                                 [](const AgentNotReadyEvent& e) { return e.agentDevice; },
                                 [](const auto&) { return std::string(); }},
                      m);
}

void setCrossRef(CstaMessage& m, const std::string& id) {
    std::visit(overloaded{[&id](DeliveredEvent& e) { e.crossRefId = id; },
                          [&id](EstablishedEvent& e) { e.crossRefId = id; },
                          [&id](ConnectionClearedEvent& e) { e.crossRefId = id; },
                          [&id](AgentReadyEvent& e) { e.crossRefId = id; },
                          [&id](AgentNotReadyEvent& e) { e.crossRefId = id; }, [](auto&) {}},
               m);
}

std::string describe(const CstaMessage& m) {
    return std::visit(
        overloaded{
            [](const DeliveredEvent& e) {
                return "DeliveredEvent call " + e.connection.callId + " at " + e.alertingDevice + " from " +
                       e.callingDevice + (e.video ? " [video]" : " [voice]");
            },
            [](const EstablishedEvent& e) {
                return "EstablishedEvent call " + e.establishedConnection.callId + " answered by " + e.answeringDevice +
                       (e.video ? " [video]" : " [voice]");
            },
            [](const ConnectionClearedEvent& e) {
                return "ConnectionClearedEvent call " + e.droppedConnection.callId + " dropped " +
                       e.droppedConnection.deviceId + " (" + e.cause + ")";
            },
            [](const AgentReadyEvent& e) { return "AgentReadyEvent " + e.agentDevice; },
            [](const AgentNotReadyEvent& e) { return "AgentNotReadyEvent " + e.agentDevice; },
            [](const CstaErrorCode& e) { return "CSTAErrorCode " + e.operation; },
            [&m](const auto&) { return messageName(m); }},
        m);
}

std::string frame(int invokeId, const std::string& xml) {
    std::size_t total = 8 + xml.size();
    if (total > 0xffff) throw CstaDecodeError("message too large for framing");
    char id[16];
    std::snprintf(id, sizeof id, "%04d", invokeId % 10000);
    std::string out;
    out += '\0';
    out += '\0';
    out += static_cast<char>((total >> 8) & 0xff);
    out += static_cast<char>(total & 0xff);
    out += id;
    out += xml;
    return out;
}

bool FrameDecoder::next(int& invokeId, std::string& xml) {
    if (buf_.size() < 8) return false;
    if (buf_[0] != '\0' || buf_[1] != '\0') throw CstaDecodeError("bad frame format indicator");
    std::size_t total = (static_cast<unsigned char>(buf_[2]) << 8) | static_cast<unsigned char>(buf_[3]);
    if (total < 8) throw CstaDecodeError("bad frame length");
    if (buf_.size() < total) return false;
    invokeId = 0;
    for (int i = 4; i < 8; ++i) {
        if (buf_[i] < '0' || buf_[i] > '9') throw CstaDecodeError("bad invoke id");
        invokeId = invokeId * 10 + (buf_[i] - '0');
    }
    xml = buf_.substr(8, total - 8);
    buf_.erase(0, total);
    return true;
}

}  // namespace svcc
