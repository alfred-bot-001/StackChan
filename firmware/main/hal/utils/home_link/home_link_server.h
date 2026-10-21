/*
 * SPDX-FileCopyrightText: 2026 alfred-bot-001 (home-link fork)
 * SPDX-License-Identifier: MIT
 *
 * esp_http_server wrapper for the Home Link LAN interface: bearer auth, JSON
 * commands under /v1, and the /v1/events WebSocket (text events + binary frames).
 */
#pragma once
#include "home_link_protocol.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace home_link {

struct HandlerResult {
    int http_status = 200;
    std::string body;
    std::string content_type = "application/json";
};

class HomeLinkServer {
public:
    using CommandHandler = std::function<HandlerResult(const Parsed&)>;

    static constexpr size_t kMaxWsClients = 4;

    HomeLinkServer()  = default;
    ~HomeLinkServer() { stop(); }

    bool start(uint16_t port, std::string token);
    void stop();
    bool running() const { return _handle != nullptr; }

    void setCommandHandler(CommandHandler handler) { _handler = std::move(handler); }
    /// Called when a WebSocket client toggles camera frames.
    void setCameraToggleHandler(std::function<void(int fd, bool enabled)> handler) { _camera_toggle = std::move(handler); }

    /// Send a text frame to every WebSocket client (queued on the httpd task).
    void broadcast(std::string_view json);
    /// Send a binary frame to the given clients.
    void sendBinary(const std::vector<int>& fds, const uint8_t* data, size_t len);
    std::vector<int> cameraClients();
    size_t clientCount();

public:
    // Implementation details (used by the static httpd callbacks).
    struct Client {
        int fd       = -1;
        bool camera  = false;
    };
    void* _handle = nullptr;  // httpd_handle_t
    std::string _token;
    CommandHandler _handler;
    std::function<void(int, bool)> _camera_toggle;
    std::mutex _clients_mutex;
    std::vector<Client> _clients;

    bool addClient(int fd);
    void removeClient(int fd);
    void setClientCamera(int fd, bool enabled);
};

}  // namespace home_link
