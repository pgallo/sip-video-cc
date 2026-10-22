#include "svcc/xml.hpp"

#include <cctype>
#include <sstream>

namespace svcc {

const XmlNode* XmlNode::child(const std::string& n) const {
    for (const auto& c : children)
        if (c.name == n) return &c;
    return nullptr;
}

std::string XmlNode::path(const std::string& p) const {
    const XmlNode* cur = this;
    std::size_t start = 0;
    while (cur && start <= p.size()) {
        std::size_t slash = p.find('/', start);
        cur = cur->child(p.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return cur ? cur->text : "";
}

XmlNode& XmlNode::add(const std::string& n, const std::string& t) {
    children.push_back(XmlNode{n, {}, t, {}});
    return children.back();
}

std::string xmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string XmlNode::toString() const {
    std::ostringstream o;
    o << '<' << name;
    for (const auto& a : attributes) o << ' ' << a.first << "=\"" << xmlEscape(a.second) << '"';
    if (children.empty() && text.empty()) {
        o << "/>";
        return o.str();
    }
    o << '>' << xmlEscape(text);
    for (const auto& c : children) o << c.toString();
    o << "</" << name << '>';
    return o.str();
}

namespace {

class Reader {
public:
    explicit Reader(const std::string& s) : s_(s) {}

    XmlNode document() {
        skipMisc();
        XmlNode root = element();
        skipMisc();
        if (pos_ != s_.size()) throw XmlError("content after root element");
        return root;
    }

private:
    void skipSpace() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    void skipMisc() {
        for (;;) {
            skipSpace();
            if (s_.compare(pos_, 2, "<?") == 0) {
                auto e = s_.find("?>", pos_);
                if (e == std::string::npos) throw XmlError("unterminated prolog");
                pos_ = e + 2;
            } else if (s_.compare(pos_, 4, "<!--") == 0) {
                auto e = s_.find("-->", pos_);
                if (e == std::string::npos) throw XmlError("unterminated comment");
                pos_ = e + 3;
            } else {
                return;
            }
        }
    }

    std::string name() {
        std::size_t b = pos_;
        while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_' ||
                                    s_[pos_] == '-' || s_[pos_] == ':' || s_[pos_] == '.'))
            ++pos_;
        if (b == pos_) throw XmlError("expected a name at offset " + std::to_string(pos_));
        return s_.substr(b, pos_ - b);
    }

    static std::string local(const std::string& qname) {
        auto c = qname.find(':');
        return c == std::string::npos ? qname : qname.substr(c + 1);
    }

    std::string unescape(const std::string& t) {
        std::string out;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i] != '&') {
                out += t[i];
                continue;
            }
            auto semi = t.find(';', i);
            if (semi == std::string::npos) throw XmlError("unterminated entity");
            std::string ent = t.substr(i + 1, semi - i - 1);
            if (ent == "amp") out += '&';
            else if (ent == "lt") out += '<';
            else if (ent == "gt") out += '>';
            else if (ent == "quot") out += '"';
            else if (ent == "apos") out += '\'';
            else throw XmlError("unknown entity &" + ent + ";");
            i = semi;
        }
        return out;
    }

    XmlNode element() {
        if (pos_ >= s_.size() || s_[pos_] != '<') throw XmlError("expected '<'");
        ++pos_;
        std::string qname = name();
        XmlNode node;
        node.name = local(qname);
        for (;;) {
            skipSpace();
            if (pos_ >= s_.size()) throw XmlError("unterminated start tag");
            if (s_.compare(pos_, 2, "/>") == 0) {
                pos_ += 2;
                return node;
            }
            if (s_[pos_] == '>') {
                ++pos_;
                break;
            }
            std::string an = name();
            skipSpace();
            if (pos_ >= s_.size() || s_[pos_] != '=') throw XmlError("expected '=' after attribute");
            ++pos_;
            skipSpace();
            char q = pos_ < s_.size() ? s_[pos_] : 0;
            if (q != '"' && q != '\'') throw XmlError("unquoted attribute value");
            auto end = s_.find(q, pos_ + 1);
            if (end == std::string::npos) throw XmlError("unterminated attribute value");
            node.attributes.emplace_back(an, unescape(s_.substr(pos_ + 1, end - pos_ - 1)));
            pos_ = end + 1;
        }
        for (;;) {
            auto lt = s_.find('<', pos_);
            if (lt == std::string::npos) throw XmlError("unterminated element " + qname);
            node.text += unescape(s_.substr(pos_, lt - pos_));
            pos_ = lt;
            if (s_.compare(pos_, 4, "<!--") == 0) {
                skipMisc();
                continue;
            }
            if (s_.compare(pos_, 2, "</") == 0) {
                pos_ += 2;
                std::string closing = name();
                skipSpace();
                if (closing != qname || pos_ >= s_.size() || s_[pos_] != '>')
                    throw XmlError("mismatched end tag </" + closing + "> for <" + qname + ">");
                ++pos_;
                break;
            }
            node.children.push_back(element());
        }
        // whitespace between child elements is not content
        if (!node.children.empty()) {
            bool blank = true;
            for (char c : node.text) blank = blank && std::isspace(static_cast<unsigned char>(c));
            if (blank) node.text.clear();
        }
        return node;
    }

    const std::string& s_;
    std::size_t pos_ = 0;
};

}  // namespace

XmlNode parseXml(const std::string& text) { return Reader(text).document(); }

}  // namespace svcc
