// A small XML reader and writer: elements, attributes, text and the five predefined entities.
// Enough for ECMA-323 messages; no DTDs, CDATA or processing instructions beyond the prolog.
#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace svcc {

struct XmlError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct XmlNode {
    std::string name;  // local name; a namespace prefix is stripped on parse
    std::vector<std::pair<std::string, std::string>> attributes;
    std::string text;
    std::vector<XmlNode> children;

    const XmlNode* child(const std::string& n) const;
    // Text of a descendant given as "a/b/c"; empty if missing.
    std::string path(const std::string& p) const;
    XmlNode& add(const std::string& n, const std::string& t = "");
    std::string toString() const;
};

XmlNode parseXml(const std::string& text);
std::string xmlEscape(const std::string& s);

}  // namespace svcc
