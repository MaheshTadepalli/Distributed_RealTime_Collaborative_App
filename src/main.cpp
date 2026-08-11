#include "json_util.hpp"
#include "metrics.hpp"
#include "net.hpp"
#include "postgres.hpp"
#include "redis_store.hpp"
#include "store.hpp"
#include "sync.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <thread>
#endif

namespace {

std::string env_or(const char* key, const std::string& fallback) {
    const char* value = std::getenv(key);
    return value == nullptr || *value == '\0' ? fallback : std::string(value);
}

uint16_t env_port(const char* key, uint16_t fallback) {
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return static_cast<uint16_t>(std::stoi(value));
}

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string base64_encode(const uint8_t* data, std::size_t size) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < size; i += 3) {
        const uint32_t octet_a = data[i];
        const uint32_t octet_b = i + 1 < size ? data[i + 1] : 0;
        const uint32_t octet_c = i + 2 < size ? data[i + 2] : 0;
        const uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        out.push_back(table[(triple >> 18) & 0x3f]);
        out.push_back(table[(triple >> 12) & 0x3f]);
        out.push_back(i + 1 < size ? table[(triple >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < size ? table[triple & 0x3f] : '=');
    }
    return out;
}

uint32_t left_rotate(uint32_t value, uint32_t count) {
    return (value << count) | (value >> (32 - count));
}

std::array<uint8_t, 20> sha1(const std::string& input) {
    uint64_t bit_length = static_cast<uint64_t>(input.size()) * 8;
    std::vector<uint8_t> data(input.begin(), input.end());
    data.push_back(0x80);
    while ((data.size() % 64) != 56) {
        data.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        data.push_back(static_cast<uint8_t>((bit_length >> (i * 8)) & 0xff));
    }

    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xefcdab89;
    uint32_t h2 = 0x98badcfe;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xc3d2e1f0;

    for (std::size_t chunk = 0; chunk < data.size(); chunk += 64) {
        std::array<uint32_t, 80> w{};
        for (int i = 0; i < 16; ++i) {
            const auto offset = chunk + static_cast<std::size_t>(i) * 4;
            w[i] = (data[offset] << 24) | (data[offset + 1] << 16) |
                   (data[offset + 2] << 8) | data[offset + 3];
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = left_rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;

        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            const uint32_t temp = left_rotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = left_rotate(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<uint8_t, 20> digest{};
    const std::array<uint32_t, 5> words{h0, h1, h2, h3, h4};
    for (std::size_t i = 0; i < words.size(); ++i) {
        digest[i * 4] = static_cast<uint8_t>((words[i] >> 24) & 0xff);
        digest[i * 4 + 1] = static_cast<uint8_t>((words[i] >> 16) & 0xff);
        digest[i * 4 + 2] = static_cast<uint8_t>((words[i] >> 8) & 0xff);
        digest[i * 4 + 3] = static_cast<uint8_t>(words[i] & 0xff);
    }
    return digest;
}

std::string websocket_accept_key(const std::string& client_key) {
    const auto digest = sha1(client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return base64_encode(digest.data(), digest.size());
}

bool header_value(const std::string& request, const std::string& name, std::string& result) {
    std::istringstream lines(request);
    std::string line;
    const std::string prefix = name + ":";
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() >= prefix.size() &&
            std::equal(prefix.begin(), prefix.end(), line.begin(),
                       [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
            auto value = line.substr(prefix.size());
            value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                                    [](unsigned char c) { return !std::isspace(c); }));
            result = value;
            return true;
        }
    }
    return false;
}

std::string encode_frame(const std::string& payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xffff) {
        frame.push_back(126);
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
        frame.push_back(static_cast<char>(payload.size() & 0xff));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((payload.size() >> (i * 8)) & 0xff));
        }
    }
    frame += payload;
    return frame;
}

bool read_frame(socket_t socket, std::string& payload) {
    uint8_t header[2]{};
    if (!collab::recv_exact(socket, header, 2)) {
        return false;
    }
    const uint8_t opcode = header[0] & 0x0f;
    if (opcode == 0x8) {
        return false;
    }
    uint64_t length = header[1] & 0x7f;
    if (length == 126) {
        uint8_t ext[2]{};
        if (!collab::recv_exact(socket, ext, 2)) return false;
        length = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (length == 127) {
        uint8_t ext[8]{};
        if (!collab::recv_exact(socket, ext, 8)) return false;
        length = 0;
        for (uint8_t byte : ext) length = (length << 8) | byte;
    }
    std::array<uint8_t, 4> mask{};
    if ((header[1] & 0x80) != 0) {
        if (!collab::recv_exact(socket, mask.data(), 4)) return false;
    }
    payload.assign(static_cast<std::size_t>(length), '\0');
    if (length > 0 && !collab::recv_exact(socket, &payload[0], static_cast<std::size_t>(length))) {
        return false;
    }
    if ((header[1] & 0x80) != 0) {
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        }
    }
    return true;
}

struct Client {
    socket_t socket{invalid_socket};
    std::string id;
};

std::unique_ptr<collab::DocumentStore> store;
std::unique_ptr<collab::PostgresStore> postgres;
collab::Metrics metrics;
std::vector<std::shared_ptr<Client>> clients;
collab::Mutex clients_mutex;
std::atomic_size_t next_client{1};
std::string document_id = "default";
constexpr std::size_t kCatchUpSnapshotThreshold = 500;

// Deduplicate local commit broadcast vs Redis Pub/Sub echo.
collab::Mutex seen_ops_mutex;
std::deque<std::string> seen_op_ids;
std::unordered_set<std::string> seen_op_id_set;
constexpr std::size_t kSeenOpLimit = 4096;

bool mark_seen_op(const std::string& op_id) {
    if (op_id.empty()) {
        return false;
    }
    collab::LockGuard lock(seen_ops_mutex);
    if (!seen_op_id_set.insert(op_id).second) {
        return false; // already seen
    }
    seen_op_ids.push_back(op_id);
    while (seen_op_ids.size() > kSeenOpLimit) {
        seen_op_id_set.erase(seen_op_ids.front());
        seen_op_ids.pop_front();
    }
    return true;
}

void broadcast(const std::string& payload) {
    const auto frame = encode_frame(payload);
    std::vector<std::shared_ptr<Client>> snapshot;
    {
        collab::LockGuard lock(clients_mutex);
        snapshot = clients;
    }
    for (const auto& client : snapshot) {
        collab::send_all(client->socket, frame);
    }
}

void broadcast_except(const std::shared_ptr<Client>& except, const std::string& payload) {
    const auto frame = encode_frame(payload);
    std::vector<std::shared_ptr<Client>> snapshot;
    {
        collab::LockGuard lock(clients_mutex);
        snapshot = clients;
    }
    for (const auto& client : snapshot) {
        if (except && client.get() == except.get()) {
            continue;
        }
        collab::send_all(client->socket, frame);
    }
}

void broadcast_op(const collab::Operation& op) {
    if (!mark_seen_op(op.op_id)) {
        return;
    }
    broadcast(collab::op_to_json(op));
}

void remove_client(const std::shared_ptr<Client>& client) {
    collab::LockGuard lock(clients_mutex);
    clients.erase(std::remove_if(clients.begin(), clients.end(),
                                 [&](const auto& item) { return item.get() == client.get(); }),
                  clients.end());
}

void send_catch_up(socket_t socket, std::size_t from_revision) {
    auto snap = store->snapshot();
    if (snap.revision < from_revision) {
        std::ostringstream err;
        err << "{\"kind\":\"error\",\"message\":\"client revision ahead of server\"}";
        collab::send_all(socket, encode_frame(err.str()));
        return;
    }

    if (snap.revision - from_revision > kCatchUpSnapshotThreshold) {
        std::ostringstream init;
        init << "{\"kind\":\"snapshot\",\"revision\":" << snap.revision
             << ",\"document\":\"" << collab::json_escape(snap.document) << "\"}";
        collab::send_all(socket, encode_frame(init.str()));
        return;
    }

    auto ops = store->operations_since(from_revision);
    if (ops.empty() && postgres && postgres->enabled()) {
        ops = postgres->operations_since(document_id, from_revision);
    }

    std::ostringstream out;
    out << "{\"kind\":\"catchup\",\"fromRevision\":" << from_revision
        << ",\"toRevision\":" << snap.revision << ",\"ops\":[";
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (i > 0) out << ",";
        out << collab::op_to_json(ops[i]);
    }
    out << "]}";
    collab::send_all(socket, encode_frame(out.str()));
}

void websocket_session(socket_t socket, const std::string& request) {
    std::string key;
    if (!header_value(request, "Sec-WebSocket-Key", key)) {
        collab::close_socket(socket);
        return;
    }

    std::ostringstream response;
    response << "HTTP/1.1 101 Switching Protocols\r\n"
             << "Upgrade: websocket\r\n"
             << "Connection: Upgrade\r\n"
             << "Sec-WebSocket-Accept: " << websocket_accept_key(key) << "\r\n\r\n";
    if (!collab::send_all(socket, response.str())) {
        collab::close_socket(socket);
        return;
    }

    auto client = std::make_shared<Client>();
    client->socket = socket;
    client->id = "user-" + std::to_string(next_client++);
    {
        collab::LockGuard lock(clients_mutex);
        clients.push_back(client);
    }
    metrics.inc_clients();

    {
        const auto snap = store->snapshot();
        std::ostringstream init;
        init << "{\"kind\":\"init\",\"clientId\":\"" << client->id
             << "\",\"revision\":" << snap.revision
             << ",\"document\":\"" << collab::json_escape(snap.document) << "\"}";
        collab::send_all(socket, encode_frame(init.str()));
    }

    std::string message;
    while (read_frame(socket, message)) {
        const auto kind = collab::json_string(message, "kind");
        if (kind == "sync") {
            const auto from_revision = collab::json_size(message, "revision");
            send_catch_up(socket, from_revision);
            continue;
        }
        if (kind != "op") {
            continue;
        }

        collab::Operation op = collab::op_from_json(message);
        op.client_id = client->id;

        try {
            const auto started = collab::now_ms();
            auto committed = store->commit(op);
            metrics.inc_ops();
            metrics.observe_latency_ms(collab::now_ms() - started);

            const auto payload = collab::op_to_json(committed);
            const auto frame = encode_frame(payload);
            // Always ACK the requesting client (including idempotent retries).
            collab::send_all(socket, frame);
            // First time we see this opId: fan out to other local clients.
            // Pub/Sub covers other replicas; echo is dropped by mark_seen_op.
            if (mark_seen_op(committed.op_id)) {
                broadcast_except(client, payload);
            }
        } catch (const std::exception& ex) {
            std::ostringstream err;
            err << "{\"kind\":\"error\",\"message\":\"" << collab::json_escape(ex.what()) << "\"}";
            collab::send_all(socket, encode_frame(err.str()));
        }
    }

    remove_client(client);
    metrics.dec_clients();
    collab::close_socket(socket);
}

void write_http(socket_t socket, const std::string& status, const std::string& content_type,
                const std::string& body) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    collab::send_all(socket, out.str());
    collab::close_socket(socket);
}

void http_response(socket_t socket, const std::string& request) {
    std::string path = "/";
    if (request.rfind("GET ", 0) == 0) {
        const auto end = request.find(' ', 4);
        path = request.substr(4, end - 4);
    }
    const auto query = path.find('?');
    if (query != std::string::npos) {
        path = path.substr(0, query);
    }

    if (path == "/healthz" || path == "/health") {
        write_http(socket, "200 OK", "text/plain; charset=utf-8", "ok\n");
        return;
    }
    if (path == "/readyz" || path == "/ready") {
        const bool ok = store && store->healthy() && (!postgres || postgres->healthy());
        write_http(socket, ok ? "200 OK" : "503 Service Unavailable", "text/plain; charset=utf-8",
                   ok ? "ready\n" : "not-ready\n");
        return;
    }
    if (path == "/metrics") {
        const auto revision = store ? store->snapshot().revision : 0;
        write_http(socket, "200 OK", "text/plain; version=0.0.4; charset=utf-8",
                   metrics.render_prometheus(revision));
        return;
    }

    if (path == "/") path = "/index.html";
    if (path.find("..") != std::string::npos) path = "/index.html";

    const auto file_path = std::string("public/") + path.substr(1);
    std::ifstream probe(file_path, std::ios::binary);
    const bool found = probe.good();
    probe.close();
    const auto body = found ? read_file(file_path) : std::string("<h1>Not found</h1>");
    const auto status = found ? "200 OK" : "404 Not Found";
    const auto has_suffix = [](const std::string& value, const std::string& suffix) {
        return value.size() >= suffix.size() &&
               value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    const auto content_type = has_suffix(path, ".css") ? "text/css" :
                              has_suffix(path, ".js") ? "application/javascript" : "text/html";
    write_http(socket, status, std::string(content_type) + "; charset=utf-8", body);
}

void handle_client(socket_t socket) {
    std::string request;
    std::array<char, 4096> buffer{};
    while (request.find("\r\n\r\n") == std::string::npos) {
        const int received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0) {
            collab::close_socket(socket);
            return;
        }
        request.append(buffer.data(), static_cast<std::size_t>(received));
        if (request.size() > 16384) {
            collab::close_socket(socket);
            return;
        }
    }

    if (request.find("Upgrade: websocket") != std::string::npos ||
        request.find("upgrade: websocket") != std::string::npos) {
        websocket_session(socket, request);
    } else {
        http_response(socket, request);
    }
}

#ifdef _WIN32
DWORD WINAPI client_thread_entry(LPVOID parameter) {
    auto* accepted_socket = static_cast<socket_t*>(parameter);
    handle_client(*accepted_socket);
    delete accepted_socket;
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::stoi(argv[1]))
                                   : env_port("PORT", 8080);
    document_id = env_or("DOCUMENT_ID", "default");
    const auto redis_host = env_or("REDIS_HOST", "");
    const auto redis_port = env_port("REDIS_PORT", 6379);
    const auto database_url = env_or("DATABASE_URL", "");

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "Failed to start Winsock\n";
        return 1;
    }
#endif

    if (!database_url.empty()) {
        postgres = std::make_unique<collab::PostgresStore>(database_url);
        if (!postgres->connect()) {
            std::cerr << "Failed to connect to PostgreSQL\n";
            return 1;
        }
        std::cout << "PostgreSQL persistence enabled\n";
    }

    if (!redis_host.empty()) {
        auto redis = std::make_unique<collab::RedisStore>(
            redis_host, redis_port, document_id, postgres.get());
        if (!redis->start()) {
            std::cerr << "Failed to start Redis store\n";
            return 1;
        }
        redis->set_broadcast_handler(broadcast_op);
        store = std::move(redis);
        std::cout << "Redis shared state enabled at " << redis_host << ":" << redis_port << "\n";
    } else {
        store = std::make_unique<collab::MemoryStore>();
        std::cout << "In-memory store (set REDIS_HOST for distributed mode)\n";
    }

    socket_t server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == invalid_socket) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    int enabled = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        std::cerr << "Could not bind to port " << port << "\n";
        collab::close_socket(server);
        return 1;
    }
    if (listen(server, SOMAXCONN) < 0) {
        std::cerr << "Could not listen on port " << port << "\n";
        collab::close_socket(server);
        return 1;
    }

    std::cout << "Collaborative editor running at http://localhost:" << port << "\n";
    std::cout << "Health: /healthz  Ready: /readyz  Metrics: /metrics\n";

    while (true) {
        sockaddr_in client_address{};
        socklen_t client_size = sizeof(client_address);
        socket_t client = accept(server, reinterpret_cast<sockaddr*>(&client_address), &client_size);
        if (client == invalid_socket) {
            continue;
        }
#ifdef _WIN32
        auto* accepted = new socket_t(client);
        CreateThread(nullptr, 0, client_thread_entry, accepted, 0, nullptr);
#else
        std::thread(handle_client, client).detach();
#endif
    }
}
