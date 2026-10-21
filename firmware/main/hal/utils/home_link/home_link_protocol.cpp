/*
 * SPDX-FileCopyrightText: 2026 alfred-bot-001 (home-link fork)
 * SPDX-License-Identifier: MIT
 */
#include "home_link_protocol.h"
#include <ArduinoJson.hpp>
#include <cctype>
#include <cstring>

namespace home_link {

namespace {

constexpr std::string_view kBearer = "Bearer ";

struct Route {
    const char* method;
    const char* path;
    Command command;
};

constexpr Route kRoutes[] = {
    {"GET", "/v1/status", Command::Status},
    {"POST", "/v1/system/reboot", Command::Reboot},
    {"POST", "/v1/system/open-home", Command::OpenHome},
    {"POST", "/v1/audio/volume", Command::Volume},
    {"POST", "/v1/display/brightness", Command::Brightness},
    {"POST", "/v1/led", Command::Led},
    {"POST", "/v1/sound", Command::Sound},
    {"POST", "/v1/avatar", Command::Avatar},
    {"POST", "/v1/avatar/emotion", Command::Emotion},
    {"POST", "/v1/avatar/speech", Command::Speech},
    {"POST", "/v1/motion", Command::Motion},
    {"POST", "/v1/motion/home", Command::MotionHome},
    {"POST", "/v1/motion/look", Command::MotionLook},
    {"POST", "/v1/motion/torque", Command::MotionTorque},
    {"POST", "/v1/neon", Command::Neon},
    {"POST", "/v1/dance", Command::Dance},
    {"DELETE", "/v1/modifiers", Command::ClearModifiers},
    {"GET", "/v1/camera/snapshot", Command::Snapshot},
    {"GET", "/v1/events", Command::Events},
};

constexpr const char* kEmotions[] = {"neutral", "happy", "angry", "sad", "doubt", "sleepy"};
constexpr const char* kSounds[]   = {"new_notification", "camera_shutter"};

Parsed fail(int status, const char* code, std::string message)
{
    Parsed p;
    p.ok          = false;
    p.http_status = status;
    p.error_code  = code;
    p.message     = std::move(message);
    return p;
}

Parsed bad(std::string message)
{
    return fail(400, "bad_request", std::move(message));
}

bool in_list(std::string_view value, const char* const* list, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (value == list[i]) return true;
    }
    return false;
}

// Reads an integer field; returns false (and sets message) when present but invalid.
bool read_int(ArduinoJson::JsonObjectConst obj, const char* key, int min, int max, int& out, bool& present,
              std::string& message)
{
    present = false;
    auto v  = obj[key];
    if (v.isNull()) return true;
    if (!v.is<int>() || v.is<bool>()) {
        message = std::string(key) + " must be an integer";
        return false;
    }
    int value = v.as<int>();
    if (value < min || value > max) {
        message = std::string(key) + " out of range";
        return false;
    }
    out     = value;
    present = true;
    return true;
}

bool read_float(ArduinoJson::JsonObjectConst obj, const char* key, float min, float max, float& out,
                std::string& message)
{
    auto v = obj[key];
    if (v.isNull() || !v.is<float>() || v.is<bool>()) {
        message = std::string(key) + " must be a number";
        return false;
    }
    float value = v.as<float>();
    if (value < min || value > max) {
        message = std::string(key) + " out of range";
        return false;
    }
    out = value;
    return true;
}

}  // namespace

bool validate_bearer(std::string_view header_value, std::string_view token)
{
    if (token.empty() || header_value.size() != kBearer.size() + token.size()) return false;
    if (header_value.substr(0, kBearer.size()) != kBearer) return false;
    const std::string_view given = header_value.substr(kBearer.size());
    unsigned char diff           = 0;
    for (size_t i = 0; i < token.size(); ++i) {
        diff |= static_cast<unsigned char>(given[i] ^ token[i]);
    }
    return diff == 0;
}

bool valid_command_id(std::string_view id)
{
    if (id.size() < 8 || id.size() > 64) return false;
    if (!std::isalnum(static_cast<unsigned char>(id[0]))) return false;
    for (unsigned char c : id) {
        if (!(std::isalnum(c) || c == '_' || c == '.' || c == ':' || c == '-')) return false;
    }
    return true;
}

bool is_home_only(Command command)
{
    switch (command) {
        case Command::Avatar:
        case Command::Emotion:
        case Command::Speech:
        case Command::Motion:
        case Command::MotionHome:
        case Command::MotionLook:
        case Command::MotionTorque:
        case Command::Neon:
        case Command::Dance:
        case Command::ClearModifiers:
        case Command::Snapshot:
            return true;
        default:
            return false;
    }
}

const char* command_name(Command command)
{
    switch (command) {
        case Command::Status: return "status";
        case Command::Reboot: return "system.reboot";
        case Command::OpenHome: return "system.open_home";
        case Command::Volume: return "audio.set_volume";
        case Command::Brightness: return "display.set_brightness";
        case Command::Led: return "led.set";
        case Command::Sound: return "sound.play";
        case Command::Avatar: return "avatar.set";
        case Command::Emotion: return "avatar.emotion";
        case Command::Speech: return "avatar.speech";
        case Command::Motion: return "motion.move";
        case Command::MotionHome: return "motion.home";
        case Command::MotionLook: return "motion.look";
        case Command::MotionTorque: return "motion.torque";
        case Command::Neon: return "neon.set";
        case Command::Dance: return "dance.play";
        case Command::ClearModifiers: return "modifiers.clear";
        case Command::Snapshot: return "camera.snapshot";
        case Command::Events: return "events";
        default: return "none";
    }
}

Parsed parse_request(std::string_view method, std::string_view uri, std::string_view body)
{
    const size_t query = uri.find('?');
    const std::string_view path = query == std::string_view::npos ? uri : uri.substr(0, query);

    bool path_known = false;
    Command command = Command::None;
    for (const auto& route : kRoutes) {
        if (path == route.path) {
            path_known = true;
            if (method == route.method) {
                command = route.command;
                break;
            }
        }
    }
    if (!path_known) return fail(404, "not_found", "unknown path");
    if (command == Command::None) return fail(405, "method_not_allowed", "method not allowed for this path");
    if (body.size() > kMaxBodyBytes) return bad("body larger than 4096 bytes");

    Parsed p;
    p.command     = command;
    p.ok          = true;
    p.http_status = 200;

    const bool wants_body = command != Command::Status && command != Command::Snapshot && command != Command::Events;
    if (!wants_body) return p;

    ArduinoJson::JsonDocument doc;
    bool have_body = false;
    for (unsigned char c : body) {
        if (!std::isspace(c)) {
            have_body = true;
            break;
        }
    }
    if (have_body) {
        auto err = ArduinoJson::deserializeJson(doc, body.data(), body.size());
        if (err) return bad("body is not valid JSON");
    }

    ArduinoJson::JsonObjectConst obj;
    if (command == Command::Dance) {
        if (!have_body || !doc.is<ArduinoJson::JsonArrayConst>()) return bad("dance body must be a JSON array");
        if (doc.as<ArduinoJson::JsonArrayConst>().size() == 0) return bad("dance sequence is empty");
        p.args.raw_json.assign(body.data(), body.size());
        return p;
    }
    if (have_body) {
        if (!doc.is<ArduinoJson::JsonObjectConst>()) return bad("body must be a JSON object");
        obj = doc.as<ArduinoJson::JsonObjectConst>();
        auto id = obj["command_id"];
        if (!id.isNull()) {
            if (!id.is<const char*>() || !valid_command_id(id.as<const char*>())) return bad("invalid command_id");
            p.command_id = id.as<const char*>();
        }
    }

    std::string msg;
    bool present = false;
    auto& a      = p.args;
    switch (command) {
        case Command::Reboot:
        case Command::OpenHome:
        case Command::ClearModifiers:
            return p;
        case Command::Volume:
            if (!read_int(obj, "volume", 0, 100, a.volume, present, msg)) return bad(msg);
            if (!present) return bad("volume is required");
            return p;
        case Command::Brightness:
            if (!read_int(obj, "brightness", 0, 100, a.brightness, present, msg)) return bad(msg);
            if (!present) return bad("brightness is required");
            return p;
        case Command::Led: {
            auto leds = obj["leds"];
            if (!leds.isNull()) {
                if (!leds.is<ArduinoJson::JsonArrayConst>()) return bad("leds must be an array");
                auto arr = leds.as<ArduinoJson::JsonArrayConst>();
                if (arr.size() == 0 || arr.size() > static_cast<size_t>(kLedCount)) return bad("leds must have 1-12 items");
                for (ArduinoJson::JsonObjectConst item : arr) {
                    LedPixel px;
                    bool pi = false, pr = false, pg = false, pb = false;
                    if (!read_int(item, "index", 0, kLedCount - 1, px.index, pi, msg) ||
                        !read_int(item, "r", 0, kLedChannelMax, px.r, pr, msg) ||
                        !read_int(item, "g", 0, kLedChannelMax, px.g, pg, msg) ||
                        !read_int(item, "b", 0, kLedChannelMax, px.b, pb, msg))
                        return bad(msg);
                    if (!(pi && pr && pg && pb)) return bad("each led needs index, r, g, b");
                    a.leds.push_back(px);
                }
                return p;
            }
            bool pr = false, pg = false, pb = false;
            if (!read_int(obj, "r", 0, kLedChannelMax, a.r, pr, msg) || !read_int(obj, "g", 0, kLedChannelMax, a.g, pg, msg) ||
                !read_int(obj, "b", 0, kLedChannelMax, a.b, pb, msg))
                return bad(msg);
            if (!(pr && pg && pb)) return bad("led needs r, g, b or leds[]");
            return p;
        }
        case Command::Sound: {
            auto name = obj["name"];
            if (!name.is<const char*>() || !in_list(name.as<const char*>(), kSounds, 2)) return bad("unknown sound name");
            a.sound = name.as<const char*>();
            return p;
        }
        case Command::Avatar:
            if (!have_body || obj.size() == 0) return bad("avatar body is empty");
            if (obj["leftEye"].isNull() && obj["rightEye"].isNull() && obj["mouth"].isNull())
                return bad("avatar needs leftEye, rightEye or mouth");
            a.raw_json.assign(body.data(), body.size());
            return p;
        case Command::Emotion: {
            auto emotion = obj["emotion"];
            if (!emotion.is<const char*>() || !in_list(emotion.as<const char*>(), kEmotions, 6)) return bad("unknown emotion");
            a.emotion      = emotion.as<const char*>();
            a.duration_ms  = 3000;
            if (!read_int(obj, "duration_ms", 0, 600000, a.duration_ms, present, msg)) return bad(msg);
            return p;
        }
        case Command::Speech: {
            auto text = obj["text"];
            if (!text.is<const char*>()) return bad("text is required");
            a.text = text.as<const char*>();
            if (a.text.empty() || a.text.size() > kMaxSpeechBytes) return bad("text must be 1-120 bytes");
            a.duration_ms = 6000;
            if (!read_int(obj, "duration_ms", 500, 60000, a.duration_ms, present, msg)) return bad(msg);
            return p;
        }
        case Command::Motion: {
            bool py = false, pp = false;
            if (!read_int(obj, "yaw", -128, 128, a.yaw, py, msg) || !read_int(obj, "pitch", 0, 90, a.pitch, pp, msg) ||
                !read_int(obj, "speed", 100, 1000, a.speed, present, msg))
                return bad(msg);
            if (!py && !pp) return bad("motion needs yaw or pitch");
            return p;
        }
        case Command::MotionHome:
            a.speed = 500;
            if (!read_int(obj, "speed", 100, 1000, a.speed, present, msg)) return bad(msg);
            return p;
        case Command::MotionLook:
            if (!read_float(obj, "x", -1.0f, 1.0f, a.x, msg) || !read_float(obj, "y", -1.0f, 1.0f, a.y, msg)) return bad(msg);
            a.has_xy = true;
            a.speed  = 500;
            if (!read_int(obj, "speed", 100, 1000, a.speed, present, msg)) return bad(msg);
            return p;
        case Command::MotionTorque: {
            auto enabled = obj["enabled"];
            if (!enabled.is<bool>()) return bad("enabled must be a boolean");
            a.torque = enabled.as<bool>() ? 1 : 0;
            return p;
        }
        case Command::Neon:
            if (!have_body || obj.size() == 0) return bad("neon body is empty");
            a.raw_json.assign(body.data(), body.size());
            return p;
        default:
            return p;
    }
}

std::string error_json(std::string_view code, std::string_view message)
{
    ArduinoJson::JsonDocument doc;
    doc["error"]   = code;
    doc["message"] = message;
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return out;
}

std::string error_json_with_app(std::string_view code, std::string_view message, std::string_view current_app)
{
    ArduinoJson::JsonDocument doc;
    doc["error"]       = code;
    doc["message"]     = message;
    doc["current_app"] = current_app;
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return out;
}

std::string result_json(std::string_view command_id, std::string_view state)
{
    ArduinoJson::JsonDocument doc;
    if (!command_id.empty()) doc["command_id"] = command_id;
    doc["state"] = state;
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return out;
}

std::string make_token_hex(const uint8_t* bytes, size_t len)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 0x0F]);
    }
    return out;
}

}  // namespace home_link
