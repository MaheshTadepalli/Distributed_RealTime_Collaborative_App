#pragma once

#include "net.hpp"

#include <array>
#include <sstream>
#include <string>
#include <vector>

namespace collab {

struct RedisValue {
    enum class Type { Nil, Status, Error, Integer, Bulk, Array };
    Type type{Type::Nil};
    std::string bulk;
    long long integer{0};
    std::vector<RedisValue> array;

    bool ok() const { return type != Type::Error && type != Type::Nil; }
    std::string as_string() const {
        if (type == Type::Bulk || type == Type::Status) return bulk;
        if (type == Type::Integer) return std::to_string(integer);
        return {};
    }
};

class RedisClient {
public:
    RedisClient() = default;
    ~RedisClient() { disconnect(); }

    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;

    bool connect(const std::string& host, uint16_t port) {
        disconnect();
        socket_ = connect_tcp(host, port);
        return socket_ != invalid_socket;
    }

    void disconnect() {
        if (socket_ != invalid_socket) {
            close_socket(socket_);
            socket_ = invalid_socket;
        }
        buffer_.clear();
    }

    bool connected() const { return socket_ != invalid_socket; }

    RedisValue command(const std::vector<std::string>& args) {
        if (!connected()) {
            return error_value("not connected");
        }
        std::ostringstream out;
        out << "*" << args.size() << "\r\n";
        for (const auto& arg : args) {
            out << "$" << arg.size() << "\r\n" << arg << "\r\n";
        }
        if (!send_all(socket_, out.str())) {
            disconnect();
            return error_value("send failed");
        }
        return read_value();
    }

    // For SUBSCRIBE: send command then read push messages via next_message().
    bool subscribe(const std::string& channel) {
        auto reply = command({"SUBSCRIBE", channel});
        return reply.type == RedisValue::Type::Array;
    }

    RedisValue next_message() { return read_value(); }

    RedisValue eval(const std::string& script, const std::vector<std::string>& keys,
                    const std::vector<std::string>& args) {
        std::vector<std::string> cmd;
        cmd.reserve(3 + keys.size() + args.size());
        cmd.push_back("EVAL");
        cmd.push_back(script);
        cmd.push_back(std::to_string(keys.size()));
        cmd.insert(cmd.end(), keys.begin(), keys.end());
        cmd.insert(cmd.end(), args.begin(), args.end());
        return command(cmd);
    }

private:
    static RedisValue error_value(const std::string& message) {
        RedisValue value;
        value.type = RedisValue::Type::Error;
        value.bulk = message;
        return value;
    }

    bool ensure_buffer(std::size_t needed) {
        while (buffer_.size() < needed) {
            std::array<char, 4096> chunk{};
            const int received = recv(socket_, chunk.data(), static_cast<int>(chunk.size()), 0);
            if (received <= 0) {
                disconnect();
                return false;
            }
            buffer_.append(chunk.data(), static_cast<std::size_t>(received));
        }
        return true;
    }

    bool read_line(std::string& line) {
        while (true) {
            const auto pos = buffer_.find("\r\n");
            if (pos != std::string::npos) {
                line = buffer_.substr(0, pos);
                buffer_.erase(0, pos + 2);
                return true;
            }
            std::array<char, 4096> chunk{};
            const int received = recv(socket_, chunk.data(), static_cast<int>(chunk.size()), 0);
            if (received <= 0) {
                disconnect();
                return false;
            }
            buffer_.append(chunk.data(), static_cast<std::size_t>(received));
        }
    }

    RedisValue read_value() {
        std::string line;
        if (!read_line(line) || line.empty()) {
            return error_value("read failed");
        }

        RedisValue value;
        const char prefix = line[0];
        const std::string payload = line.substr(1);

        if (prefix == '+') {
            value.type = RedisValue::Type::Status;
            value.bulk = payload;
            return value;
        }
        if (prefix == '-') {
            value.type = RedisValue::Type::Error;
            value.bulk = payload;
            return value;
        }
        if (prefix == ':') {
            value.type = RedisValue::Type::Integer;
            value.integer = std::stoll(payload);
            return value;
        }
        if (prefix == '$') {
            const auto size = std::stoll(payload);
            if (size < 0) {
                value.type = RedisValue::Type::Nil;
                return value;
            }
            if (!ensure_buffer(static_cast<std::size_t>(size) + 2)) {
                return error_value("bulk read failed");
            }
            value.type = RedisValue::Type::Bulk;
            value.bulk = buffer_.substr(0, static_cast<std::size_t>(size));
            buffer_.erase(0, static_cast<std::size_t>(size) + 2);
            return value;
        }
        if (prefix == '*') {
            const auto count = std::stoll(payload);
            value.type = RedisValue::Type::Array;
            if (count < 0) {
                value.type = RedisValue::Type::Nil;
                return value;
            }
            value.array.reserve(static_cast<std::size_t>(count));
            for (long long i = 0; i < count; ++i) {
                value.array.push_back(read_value());
            }
            return value;
        }
        return error_value("unknown RESP type");
    }

    socket_t socket_{invalid_socket};
    std::string buffer_;
};

} // namespace collab
