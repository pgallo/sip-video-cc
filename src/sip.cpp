#include "svcc/sip.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace svcc {

namespace {

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

std::string canonical(const std::string& name) {
    static const std::map<std::string, std::string> compact = {
        {"i", "Call-ID"}, {"f", "From"}, {"t", "To"}, {"v", "Via"}, {"m", "Contact"},
        {"l", "Content-Length"}, {"c", "Content-Type"}};
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    auto it = compact.find(lower);
    return it == compact.end() ? name : it->second;
}

}  // namespace

std::string SipMessage::header(const std::string& name) const {
    for (const auto& h : headers)
        if (iequals(h.first, name)) return h.second;
    return "";
}

void SipMessage::set(const std::string& name, const std::string& value) {
    for (auto& h : headers)
        if (iequals(h.first, name)) {
            h.second = value;
            return;
        }
    headers.emplace_back(name, value);
}

void SipMessage::add(const std::string& name, const std::string& value) { headers.emplace_back(name, value); }

int SipMessage::cseq() const {
    try {
        return std::stoi(header("CSeq"));
    } catch (const std::exception&) {
        throw SipError("malformed CSeq");
    }
}

std::string SipMessage::cseqMethod() const {
    std::string v = header("CSeq");
    auto sp = v.find(' ');
    return sp == std::string::npos ? "" : trim(v.substr(sp + 1));
}

std::string headerParam(const std::string& value, const std::string& name) {
    std::string key = ";" + name + "=";
    auto gt = value.find('>');
    auto p = value.find(key, gt == std::string::npos ? 0 : gt);
    if (p == std::string::npos) return "";
    auto start = p + key.size();
    auto end = value.find_first_of(";,", start);
    return value.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string SipMessage::fromTag() const { return headerParam(header("From"), "tag"); }
std::string SipMessage::toTag() const { return headerParam(header("To"), "tag"); }

std::string SipMessage::serialize() const {
    std::ostringstream out;
    if (isRequest)
        out << method << ' ' << uri << " SIP/2.0\r\n";
    else
        out << "SIP/2.0 " << status << ' ' << reason << "\r\n";
    for (const auto& h : headers)
        if (!iequals(h.first, "Content-Length")) out << h.first << ": " << h.second << "\r\n";
    out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    return out.str();
}

SipMessage SipMessage::parse(const std::string& text) {
    auto end = text.find("\r\n\r\n");
    if (end == std::string::npos) throw SipError("no end of headers");
    std::istringstream head(text.substr(0, end));
    std::string line;
    std::getline(head, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    SipMessage m;
    if (line.rfind("SIP/2.0 ", 0) == 0) {
        m.isRequest = false;
        if (line.size() < 11) throw SipError("bad status line");
        try {
            m.status = std::stoi(line.substr(8, 3));
        } catch (const std::exception&) {
            throw SipError("bad status code");
        }
        if (m.status < 100 || m.status > 699) throw SipError("status out of range");
        m.reason = line.size() > 12 ? line.substr(12) : "";
    } else {
        auto a = line.find(' ');
        auto b = line.rfind(' ');
        if (a == std::string::npos || a == b || line.substr(b + 1) != "SIP/2.0") throw SipError("bad request line");
        m.method = line.substr(0, a);
        m.uri = line.substr(a + 1, b - a - 1);
        if (m.uri.rfind("sip:", 0) != 0) throw SipError("unsupported URI: " + m.uri);
    }
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if ((line[0] == ' ' || line[0] == '\t') && !m.headers.empty()) {
            m.headers.back().second += " " + trim(line);
            continue;
        }
        auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0) throw SipError("malformed header: " + line);
        m.headers.emplace_back(canonical(trim(line.substr(0, colon))), trim(line.substr(colon + 1)));
    }
    for (const char* required : {"Call-ID", "CSeq", "From", "To"})
        if (m.header(required).empty()) throw SipError(std::string("missing ") + required);
    std::string body = text.substr(end + 4);
    std::string cl = m.header("Content-Length");
    if (!cl.empty()) {
        std::size_t n = std::stoul(cl);
        if (n > body.size()) throw SipError("truncated body");
        body.resize(n);
    }
    m.body = body;
    m.cseq();
    return m;
}

SipMessage makeResponse(const SipMessage& req, int status, const std::string& reason, const std::string& toTag) {
    SipMessage r;
    r.isRequest = false;
    r.status = status;
    r.reason = reason;
    for (const char* h : {"Via", "From", "To", "Call-ID", "CSeq"}) {
        std::string v = req.header(h);
        if (!v.empty()) r.add(h, v);
    }
    if (!toTag.empty() && req.toTag().empty()) r.set("To", req.header("To") + ";tag=" + toTag);
    return r;
}

std::string uriUser(const std::string& uri) {
    auto colon = uri.find(':');
    auto at = uri.find('@');
    if (colon == std::string::npos || at == std::string::npos) return "";
    return uri.substr(colon + 1, at - colon - 1);
}

std::string uriHost(const std::string& uri) {
    auto at = uri.find('@');
    if (at == std::string::npos) return "";
    auto end = uri.find_first_of(";>?", at);
    return uri.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
}

void SipNetwork::send(const std::string& from, const std::string& to, const SipMessage& msg) {
    std::string wire = msg.serialize();
    if (tracing) {
        trace.push_back(from + " -> " + to + ": " +
                        (msg.isRequest ? msg.method : std::to_string(msg.status) + " " + msg.reason) +
                        (msg.isRequest ? "" : " (" + msg.cseqMethod() + ")"));
    }
    sched_.schedule(latency_, [this, from, to, wire]() {
        auto it = endpoints_.find(to);
        if (it == endpoints_.end()) return;
        ++delivered;
        it->second->onSip(SipMessage::parse(wire), from);
    });
}

}  // namespace svcc
