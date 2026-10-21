/*
 * SPDX-FileCopyrightText: 2026 alfred-bot-001 (home-link fork)
 * SPDX-License-Identifier: MIT
 */
#include "home_link_server.h"
#include <ArduinoJson.hpp>
#include <esp_http_server.h>
#include <esp_log.h>
#include <algorithm>
#include <cstring>
#include <memory>

namespace home_link {

static const char* TAG = "home_link";

namespace {

struct QueuedFrame {
    HomeLinkServer* server;
    std::vector<int> fds;
    std::vector<uint8_t> payload;
    bool binary;
};

void send_queued_frame(void* arg)
{
    std::unique_ptr<QueuedFrame> frame(static_cast<QueuedFrame*>(arg));
    auto handle = static_cast<httpd_handle_t>(frame->server->_handle);
    if (!handle) return;
    httpd_ws_frame_t pkt{};
    pkt.final   = true;
    pkt.type    = frame->binary ? HTTPD_WS_TYPE_BINARY : HTTPD_WS_TYPE_TEXT;
    pkt.payload = frame->payload.data();
    pkt.len     = frame->payload.size();
    for (int fd : frame->fds) {
        if (httpd_ws_get_fd_info(handle, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            frame->server->removeClient(fd);
            continue;
        }
        if (httpd_ws_send_frame_async(handle, fd, &pkt) != ESP_OK) {
            frame->server->removeClient(fd);
            httpd_sess_trigger_close(handle, fd);
        }
    }
}

bool authorized(HomeLinkServer* server, httpd_req_t* req)
{
    const size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len == 0 || len > 200) return false;
    std::string value(len + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, "Authorization", value.data(), value.size()) != ESP_OK) return false;
    value.resize(len);
    return validate_bearer(value, server->_token);
}

esp_err_t reply(httpd_req_t* req, int status, const std::string& body, const char* content_type = "application/json")
{
    const char* status_text = "200 OK";
    switch (status) {
        case 200: status_text = "200 OK"; break;
        case 400: status_text = "400 Bad Request"; break;
        case 401: status_text = "401 Unauthorized"; break;
        case 404: status_text = "404 Not Found"; break;
        case 405: status_text = "405 Method Not Allowed"; break;
        case 409: status_text = "409 Conflict"; break;
        case 413: status_text = "413 Payload Too Large"; break;
        case 501: status_text = "501 Not Implemented"; break;
        case 503: status_text = "503 Service Unavailable"; break;
        default: status_text = "500 Internal Server Error"; break;
    }
    httpd_resp_set_status(req, status_text);
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.data(), body.size());
}

esp_err_t http_handler(httpd_req_t* req)
{
    auto* server = static_cast<HomeLinkServer*>(req->user_ctx);
    if (!authorized(server, req)) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return reply(req, 401, error_json("unauthorized", "missing or wrong bearer token"));
    }
    if (req->content_len > kMaxBodyBytes) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return reply(req, 400, error_json("bad_request", "body larger than 4096 bytes"));
    }
    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        int n = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return ESP_FAIL;
        }
        received += static_cast<size_t>(n);
    }
    const char* method = req->method == HTTP_GET ? "GET" : req->method == HTTP_POST ? "POST" : req->method == HTTP_DELETE ? "DELETE" : "OTHER";
    Parsed parsed = parse_request(method, req->uri, body);
    if (!parsed.ok) {
        return reply(req, parsed.http_status, error_json(parsed.error_code, parsed.message));
    }
    if (!server->_handler) {
        return reply(req, 501, error_json("not_implemented", "no handler"));
    }
    HandlerResult result = server->_handler(parsed);
    return reply(req, result.http_status, result.body, result.content_type.c_str());
}

esp_err_t ws_handler(httpd_req_t* req)
{
    auto* server = static_cast<HomeLinkServer*>(req->user_ctx);
    if (req->method == HTTP_GET) {
        // Handshake request: authenticate before the upgrade completes.
        if (!authorized(server, req)) {
            httpd_resp_set_hdr(req, "Connection", "close");
            return reply(req, 401, error_json("unauthorized", "missing or wrong bearer token"));
        }
        const int fd = httpd_req_to_sockfd(req);
        if (!server->addClient(fd)) {
            httpd_resp_set_hdr(req, "Connection", "close");
            return reply(req, 503, error_json("too_many_clients", "at most 4 event clients"));
        }
        ESP_LOGI(TAG, "events client connected (fd %d)", fd);
        return ESP_OK;
    }
    httpd_ws_frame_t pkt{};
    pkt.type = HTTPD_WS_TYPE_TEXT;
    if (httpd_ws_recv_frame(req, &pkt, 0) != ESP_OK) return ESP_FAIL;
    const int fd = httpd_req_to_sockfd(req);
    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        server->removeClient(fd);
        return ESP_OK;
    }
    if (pkt.len == 0 || pkt.len > 512) {
        // Drain oversized frames without acting on them.
        return pkt.len == 0 ? ESP_OK : ESP_FAIL;
    }
    std::vector<uint8_t> buf(pkt.len + 1, 0);
    pkt.payload = buf.data();
    if (httpd_ws_recv_frame(req, &pkt, pkt.len) != ESP_OK) return ESP_FAIL;
    if (pkt.type == HTTPD_WS_TYPE_PING) {
        pkt.type = HTTPD_WS_TYPE_PONG;
        return httpd_ws_send_frame(req, &pkt);
    }
    if (pkt.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;
    ArduinoJson::JsonDocument doc;
    if (ArduinoJson::deserializeJson(doc, buf.data(), pkt.len)) return ESP_OK;
    if (doc["type"] == "camera" && doc["enabled"].is<bool>()) {
        const bool enabled = doc["enabled"].as<bool>();
        server->setClientCamera(fd, enabled);
        if (server->_camera_toggle) server->_camera_toggle(fd, enabled);
    }
    return ESP_OK;
}

void on_close(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    auto* server = static_cast<HomeLinkServer*>(httpd_get_global_user_ctx(hd));
    if (server) server->removeClient(sockfd);
    close(sockfd);
}

}  // namespace

bool HomeLinkServer::start(uint16_t port, std::string token)
{
    if (_handle) return true;
    _token = std::move(token);
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.server_port      = port;
    config.ctrl_port        = 32770;
    config.stack_size       = 8192;
    config.max_uri_handlers = 8;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.global_user_ctx  = this;
    config.global_user_ctx_free_fn = [](void*) {};
    config.close_fn         = on_close;
    httpd_handle_t handle   = nullptr;
    if (httpd_start(&handle, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return false;
    }
    _handle = handle;

    httpd_uri_t events{};
    events.uri          = "/v1/events";
    events.method       = HTTP_GET;
    events.handler      = ws_handler;
    events.user_ctx     = this;
    events.is_websocket = true;
    events.handle_ws_control_frames = true;
    httpd_register_uri_handler(handle, &events);

    for (httpd_method_t method : {HTTP_GET, HTTP_POST, HTTP_DELETE}) {
        httpd_uri_t uri{};
        uri.uri      = "/v1/*";
        uri.method   = method;
        uri.handler  = http_handler;
        uri.user_ctx = this;
        httpd_register_uri_handler(handle, &uri);
    }
    ESP_LOGI(TAG, "server started on port %u", static_cast<unsigned>(port));
    return true;
}

void HomeLinkServer::stop()
{
    if (!_handle) return;
    auto handle = static_cast<httpd_handle_t>(_handle);
    _handle     = nullptr;
    httpd_stop(handle);
    std::lock_guard<std::mutex> lock(_clients_mutex);
    _clients.clear();
    ESP_LOGI(TAG, "server stopped");
}

bool HomeLinkServer::addClient(int fd)
{
    std::lock_guard<std::mutex> lock(_clients_mutex);
    for (auto& c : _clients) {
        if (c.fd == fd) {
            c.camera = false;
            return true;
        }
    }
    if (_clients.size() >= kMaxWsClients) return false;
    _clients.push_back({fd, false});
    return true;
}

void HomeLinkServer::removeClient(int fd)
{
    std::lock_guard<std::mutex> lock(_clients_mutex);
    _clients.erase(std::remove_if(_clients.begin(), _clients.end(), [fd](const Client& c) { return c.fd == fd; }),
                   _clients.end());
}

void HomeLinkServer::setClientCamera(int fd, bool enabled)
{
    std::lock_guard<std::mutex> lock(_clients_mutex);
    for (auto& c : _clients) {
        if (c.fd == fd) c.camera = enabled;
    }
}

std::vector<int> HomeLinkServer::cameraClients()
{
    std::lock_guard<std::mutex> lock(_clients_mutex);
    std::vector<int> out;
    for (const auto& c : _clients) {
        if (c.camera) out.push_back(c.fd);
    }
    return out;
}

size_t HomeLinkServer::clientCount()
{
    std::lock_guard<std::mutex> lock(_clients_mutex);
    return _clients.size();
}

void HomeLinkServer::broadcast(std::string_view json)
{
    if (!_handle) return;
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lock(_clients_mutex);
        for (const auto& c : _clients) fds.push_back(c.fd);
    }
    if (fds.empty()) return;
    auto* frame    = new QueuedFrame{this, std::move(fds), std::vector<uint8_t>(json.begin(), json.end()), false};
    if (httpd_queue_work(static_cast<httpd_handle_t>(_handle), send_queued_frame, frame) != ESP_OK) delete frame;
}

void HomeLinkServer::sendBinary(const std::vector<int>& fds, const uint8_t* data, size_t len)
{
    if (!_handle || fds.empty() || !data || len == 0) return;
    auto* frame = new QueuedFrame{this, fds, std::vector<uint8_t>(data, data + len), true};
    if (httpd_queue_work(static_cast<httpd_handle_t>(_handle), send_queued_frame, frame) != ESP_OK) delete frame;
}

}  // namespace home_link
