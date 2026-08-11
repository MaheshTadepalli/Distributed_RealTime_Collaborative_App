#pragma once

#include "ot.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>

namespace collab {

inline std::string json_escape(const std::string& input) {
    std::ostringstream out;
    for (const unsigned char c : input) {
        switch (c) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
            } else {
                out << c;
            }
        }
    }
    return out.str();
}

inline std::string json_string(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos);
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos);
    if (pos == std::string::npos) return {};
    std::string value;
    bool escaped = false;
    for (++pos; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (escaped) {
            switch (c) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: value.push_back(c); break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            break;
        } else {
            value.push_back(c);
        }
    }
    return value;
}

inline std::size_t json_size(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return 0;
    while (++pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {}
    std::size_t value = 0;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
        value = value * 10 + static_cast<std::size_t>(json[pos] - '0');
        ++pos;
    }
    return value;
}

inline std::string op_to_json(const Operation& op, const std::string& kind = "op") {
    std::ostringstream out;
    out << "{\"kind\":\"" << kind << "\",\"type\":\""
        << (op.type == OpType::Insert ? "insert" : "delete")
        << "\",\"position\":" << op.position
        << ",\"text\":\"" << json_escape(op.text)
        << "\",\"count\":" << op.count
        << ",\"revision\":" << op.revision
        << ",\"baseRevision\":" << op.base_revision
        << ",\"clientId\":\"" << json_escape(op.client_id)
        << "\",\"opId\":\"" << json_escape(op.op_id) << "\"}";
    return out.str();
}

inline Operation op_from_json(const std::string& message) {
    Operation op;
    op.type = json_string(message, "type") == "delete" ? OpType::Delete : OpType::Insert;
    op.position = json_size(message, "position");
    op.text = json_string(message, "text");
    op.count = json_size(message, "count");
    op.base_revision = json_size(message, "baseRevision");
    op.revision = json_size(message, "revision");
    op.client_id = json_string(message, "clientId");
    op.op_id = json_string(message, "opId");
    return op;
}

} // namespace collab
