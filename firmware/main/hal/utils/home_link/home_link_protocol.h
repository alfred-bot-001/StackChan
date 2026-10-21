/*
 * SPDX-FileCopyrightText: 2026 alfred-bot-001 (home-link fork)
 * SPDX-License-Identifier: MIT
 *
 * Home Link protocol: request parsing/validation and response encoding for the
 * LAN control interface. No ESP-IDF dependency so it can be unit tested on the
 * host (see firmware/tests/home_link_protocol_test.cpp).
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace home_link {

constexpr uint16_t kPort       = 8700;
constexpr size_t kMaxBodyBytes = 4096;
constexpr int kVersion         = 1;
constexpr size_t kMaxSpeechBytes = 120;
constexpr int kLedChannelMax     = 168;
constexpr int kLedCount          = 12;

enum class Command {
    None,
    Status,
    Reboot,
    OpenHome,
    Volume,
    Brightness,
    Led,
    Sound,
    Avatar,
    Emotion,
    Speech,
    Motion,
    MotionHome,
    MotionLook,
    MotionTorque,
    Neon,
    Dance,
    ClearModifiers,
    Snapshot,
    Events,
};

struct LedPixel {
    int index = 0;
    int r     = 0;
    int g     = 0;
    int b     = 0;
};

struct CommandArgs {
    int volume     = -1;
    int brightness = -1;
    int r = -1, g = -1, b = -1;        // led.set single colour
    std::vector<LedPixel> leds;        // led.set per pixel
    std::string sound;
    std::string emotion;
    std::string text;
    std::string raw_json;              // avatar / neon / dance payload passed through to the firmware parsers
    int duration_ms = -1;
    int yaw         = -9999;           // degrees; -9999 = not given
    int pitch       = -9999;
    int speed       = 150;
    float x = 0.0f, y = 0.0f;
    bool has_xy  = false;
    int torque   = -1;                 // -1 not given, 0 off, 1 on
};

struct Parsed {
    bool ok         = false;
    int http_status = 500;
    std::string error_code;
    std::string message;
    Command command = Command::None;
    CommandArgs args;
    std::string command_id;
};

/// Constant-time check of "Authorization" header value against the token ("Bearer <token>").
bool validate_bearer(std::string_view header_value, std::string_view token);

/// Map method + URI (query string ignored) + JSON body to a validated command.
Parsed parse_request(std::string_view method, std::string_view uri, std::string_view body);

/// Commands that only work while the HOME app is active.
bool is_home_only(Command command);

/// Stable name used in logs and the status response.
const char* command_name(Command command);

std::string error_json(std::string_view code, std::string_view message);
std::string error_json_with_app(std::string_view code, std::string_view message, std::string_view current_app);
std::string result_json(std::string_view command_id, std::string_view state);

/// 2 hex chars per byte, lower case.
std::string make_token_hex(const uint8_t* bytes, size_t len);

/// Accepts 8–64 chars of [A-Za-z0-9_.:-], first char alphanumeric.
bool valid_command_id(std::string_view id);

}  // namespace home_link
