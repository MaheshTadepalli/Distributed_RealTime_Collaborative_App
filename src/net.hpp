#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using socket_t = SOCKET;
constexpr socket_t invalid_socket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t invalid_socket = -1;
#endif

namespace collab {

inline void close_socket(socket_t socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

inline bool send_all(socket_t socket, const std::string& data) {
    const char* cursor = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const int sent = send(socket, cursor, static_cast<int>(remaining), 0);
        if (sent <= 0) {
            return false;
        }
        cursor += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

inline bool recv_exact(socket_t socket, void* destination, std::size_t size) {
    auto* cursor = static_cast<char*>(destination);
    std::size_t remaining = size;
    while (remaining > 0) {
        const int received = recv(socket, cursor, static_cast<int>(remaining), 0);
        if (received <= 0) {
            return false;
        }
        cursor += received;
        remaining -= static_cast<std::size_t>(received);
    }
    return true;
}

inline socket_t connect_tcp(const std::string& host, uint16_t port) {
#ifdef _WIN32
    // Prefer inet_addr for IPv4 literals; fall back to gethostbyname for hostnames.
    socket_t sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == invalid_socket) {
        return invalid_socket;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = inet_addr(host.c_str());
    if (address.sin_addr.s_addr == INADDR_NONE) {
        hostent* entry = gethostbyname(host.c_str());
        if (entry == nullptr || entry->h_addr_list[0] == nullptr) {
            close_socket(sock);
            return invalid_socket;
        }
        std::memcpy(&address.sin_addr, entry->h_addr_list[0], sizeof(address.sin_addr));
    }
    if (connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(sock);
        return invalid_socket;
    }
    return sock;
#else
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const auto port_text = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result) != 0 || result == nullptr) {
        return invalid_socket;
    }

    socket_t sock = invalid_socket;
    for (auto* info = result; info != nullptr; info = info->ai_next) {
        sock = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (sock == invalid_socket) {
            continue;
        }
        if (connect(sock, info->ai_addr, static_cast<int>(info->ai_addrlen)) == 0) {
            break;
        }
        close_socket(sock);
        sock = invalid_socket;
    }
    freeaddrinfo(result);
    return sock;
#endif
}

} // namespace collab
