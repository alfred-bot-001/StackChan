/*
 * SPDX-FileCopyrightText: 2026 alfred-bot-001 (home-link fork)
 * SPDX-License-Identifier: MIT
 *
 * Home Link: LAN control interface for the family console. Lifecycle task,
 * status JSON, command dispatch to the HAL / StackChan objects, event push.
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include "utils/home_link/home_link_server.h"
#include <stackchan/stackchan.h>
#include <stackchan/modifiers/modifiers.h>
#include <stackchan/animation/animation.h>
#include <apps/common/common.h>
#include <mooncake_log.h>
#include <ArduinoJson.hpp>
#include <settings.h>
#include <wifi_manager.h>
#include <ssid_manager.h>
#include <board.h>
#include <jpg/image_to_jpeg.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mdns.h>
#include <atomic>
#include <mutex>

using namespace home_link;
using namespace stackchan;

static const std::string_view _tag       = "HAL-HomeLink";
static constexpr const char* _nvs_ns     = "home_link";
static constexpr const char* _nvs_token  = "token";
static constexpr const char* _nvs_enable = "enabled";
static constexpr int kHomeAppIndex       = 7;

namespace {

HomeLinkServer _server;
std::mutex _command_mutex;
std::atomic<bool> _task_started{false};
std::atomic<bool> _network_started_by_us{false};
std::atomic<bool> _config_dirty{false};
std::atomic<int> _pending_reboot{-1};
std::atomic<uint32_t> _last_motion_tick{0};
std::atomic<bool> _home_active{false};
std::string _current_app = "LAUNCHER";
std::mutex _app_mutex;
std::function<void()> _restore_default_modifiers;
bool _mdns_started = false;

std::string load_or_create_token()
{
    Settings settings(_nvs_ns, true);
    std::string token = settings.GetString(_nvs_token, "");
    if (token.size() != 32) {
        uint8_t bytes[16];
        esp_fill_random(bytes, sizeof(bytes));
        token = make_token_hex(bytes, sizeof(bytes));
        settings.SetString(_nvs_token, token);
        mclog::tagInfo(_tag, "token generated");
    }
    return token;
}

bool enabled_setting()
{
    Settings settings(_nvs_ns, false);
    return settings.GetBool(_nvs_enable, true);
}

std::string status_json()
{
    auto& hal  = GetHAL();
    auto& wifi = WifiManager::GetInstance();
    ArduinoJson::JsonDocument doc;
    doc["firmware_version"]  = FIRMWARE_VERSION;
    doc["home_link_version"] = kVersion;
    doc["uptime_ms"]         = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    doc["battery"]["level"]    = hal.getBatteryLevel();
    doc["battery"]["charging"] = hal.isBatteryCharging();
    if (wifi.IsConnected()) {
        doc["wifi"]["ssid"] = wifi.GetSsid();
        doc["wifi"]["ip"]   = wifi.GetIpAddress();
        doc["wifi"]["rssi"] = wifi.GetRssi();
    } else {
        doc["wifi"]["ssid"] = nullptr;
        doc["wifi"]["ip"]   = nullptr;
        doc["wifi"]["rssi"] = nullptr;
    }
    doc["app"]["name"]        = hal.getCurrentAppName();
    doc["app"]["home_active"] = _home_active.load();
    {
        LvglLockGuard lock;
        auto& sc     = GetStackChan();
        auto& motion = sc.motion();
        doc["motion"]["yaw"]    = motion.getCurrentYawAngle() / 10.0f;
        doc["motion"]["pitch"]  = motion.getCurrentPitchAngle() / 10.0f;
        doc["motion"]["moving"] = motion.isMoving();
        doc["motion"]["torque"] = motion.yawServo().getTorqueEnabled() || motion.pitchServo().getTorqueEnabled();
        doc["avatar_attached"]  = sc.hasAvatar();
    }
    doc["audio"]["volume"]        = hal.getSpeakerVolume();
    doc["display"]["brightness"]  = hal.getBackLightBrightness();
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return out;
}

HandlerResult json_result(int status, std::string body)
{
    return HandlerResult{status, std::move(body), "application/json"};
}

avatar::Emotion emotion_from(const std::string& name)
{
    if (name == "happy") return avatar::Emotion::Happy;
    if (name == "angry") return avatar::Emotion::Angry;
    if (name == "sad") return avatar::Emotion::Sad;
    if (name == "doubt") return avatar::Emotion::Doubt;
    if (name == "sleepy") return avatar::Emotion::Sleepy;
    return avatar::Emotion::Neutral;
}

HandlerResult snapshot_result()
{
    auto camera = hal_bridge::board_get_camera();
    if (!camera) return json_result(503, error_json("camera_unavailable", "camera not initialised"));
    if (!camera->StreamCaptures()) return json_result(503, error_json("camera_unavailable", "capture failed"));
    uint8_t* jpeg = nullptr;
    size_t len    = 0;
    const bool ok = image_to_jpeg((uint8_t*)camera->GetFrameData(), camera->GetFrameSize(), camera->GetFrameWidth(),
                                  camera->GetFrameHeight(), (v4l2_pix_fmt_t)camera->GetFrameFormat(), 20, &jpeg, &len);
    if (!ok || !jpeg) {
        free(jpeg);
        return json_result(503, error_json("camera_unavailable", "jpeg encode failed"));
    }
    HandlerResult r;
    r.http_status  = 200;
    r.content_type = "image/jpeg";
    r.body.assign(reinterpret_cast<const char*>(jpeg), len);
    free(jpeg);
    return r;
}

HandlerResult handle_command(const Parsed& p)
{
    std::lock_guard<std::mutex> guard(_command_mutex);
    auto& hal  = GetHAL();
    const auto& a = p.args;
    auto done     = [&](const char* state) { return json_result(200, result_json(p.command_id, state)); };

    if (is_home_only(p.command) && !_home_active.load()) {
        return json_result(409, error_json_with_app("home_app_not_active", "command needs the HOME app",
                                                    hal.getCurrentAppName()));
    }

    switch (p.command) {
        case Command::Status:
            return json_result(200, status_json());
        case Command::Volume:
            hal.setSpeakerVolume(static_cast<uint8_t>(a.volume), true);
            return done("completed");
        case Command::Brightness:
            hal.setBackLightBrightness(static_cast<uint8_t>(a.brightness), true);
            return done("completed");
        case Command::Led:
            if (a.leds.empty()) {
                hal.showRgbColor(static_cast<uint8_t>(a.r), static_cast<uint8_t>(a.g), static_cast<uint8_t>(a.b));
            } else {
                for (const auto& px : a.leds) hal.setRgbColor(px.index, px.r, px.g, px.b);
                hal.refreshRgb();
            }
            return done("completed");
        case Command::Sound:
            hal_bridge::app_play_sound(a.sound == "camera_shutter" ? OGG_CAMERA_SHUTTER : OGG_NEW_NOTIFICATION);
            return done("completed");
        case Command::Reboot:
            _pending_reboot = 0;
            return done("accepted");
        case Command::OpenHome:
            if (_home_active.load()) return done("completed");
            _pending_reboot = kHomeAppIndex;
            return done("accepted");
        case Command::Snapshot:
            return snapshot_result();
        default:
            break;
    }

    // HOME-only commands below: the avatar is attached while HOME is active.
    LvglLockGuard lock;
    auto& sc = GetStackChan();
    if (!sc.hasAvatar()) return json_result(409, error_json("home_app_not_active", "avatar not attached"));
    auto& motion = sc.motion();
    switch (p.command) {
        case Command::Avatar:
            sc.updateAvatarFromJson(a.raw_json.c_str());
            return done("completed");
        case Command::Emotion:
            if (a.duration_ms > 0) {
                sc.addModifier(std::make_unique<TimedEmotionModifier>(emotion_from(a.emotion), a.duration_ms));
            } else {
                sc.avatar().setEmotion(emotion_from(a.emotion));
            }
            return done("completed");
        case Command::Speech:
            sc.addModifier(std::make_unique<TimedSpeechModifier>(a.text, a.duration_ms));
            sc.addModifier(std::make_unique<SpeakingModifier>(std::min<uint32_t>(a.duration_ms, 2000)));
            return done("completed");
        case Command::Motion:
            _last_motion_tick = hal.millis();
            motion.setAutoAngleSyncEnabled(false);
            if (a.pitch != -9999) motion.pitchServo().moveWithSpeed(a.pitch * 10, a.speed);
            if (a.yaw != -9999) motion.yawServo().moveWithSpeed(a.yaw * 10, a.speed);
            return done("accepted");
        case Command::MotionHome:
            _last_motion_tick = hal.millis();
            motion.setAutoAngleSyncEnabled(false);
            motion.goHome(a.speed);
            return done("accepted");
        case Command::MotionLook:
            _last_motion_tick = hal.millis();
            motion.setAutoAngleSyncEnabled(false);
            motion.lookAtNormalized(a.x, a.y, a.speed);
            return done("accepted");
        case Command::MotionTorque:
            motion.setTorqueEnabled(a.torque == 1);
            return done("completed");
        case Command::Neon:
            sc.updateNeonLightFromJson(a.raw_json.c_str());
            return done("completed");
        case Command::Dance: {
            auto sequence = animation::parse_sequence_from_json(a.raw_json.c_str());
            if (sequence.empty()) return json_result(400, error_json("bad_request", "dance sequence could not be parsed"));
            sc.addModifier(std::make_unique<DanceModifier>(sequence));
            return done("accepted");
        }
        case Command::ClearModifiers:
            sc.clearModifiers();
            if (_restore_default_modifiers) _restore_default_modifiers();
            return done("completed");
        default:
            return json_result(501, error_json("not_implemented", "unknown command"));
    }
}

void start_mdns()
{
    if (_mdns_started) return;
    if (mdns_init() != ESP_OK) {
        mclog::tagWarn(_tag, "mdns init failed");
        return;
    }
    const std::string mac  = GetHAL().getFactoryMacString("");
    const std::string host = "stackchan-" + (mac.size() >= 4 ? mac.substr(mac.size() - 4) : mac);
    mdns_hostname_set(host.c_str());
    mdns_instance_name_set("StackChan Home Link");
    mdns_txt_item_t txt[] = {{"v", "1"}};
    mdns_service_add(nullptr, "_stackchan-home", "_tcp", kPort, txt, 1);
    _mdns_started = true;
    mclog::tagInfo(_tag, "mdns host {}.local", host);
}

void push_camera_frames()
{
    auto fds = _server.cameraClients();
    if (fds.empty() || !_home_active.load()) return;
    auto camera = hal_bridge::board_get_camera();
    if (!camera || !camera->StreamCaptures()) return;
    uint8_t* jpeg = nullptr;
    size_t len    = 0;
    if (image_to_jpeg((uint8_t*)camera->GetFrameData(), camera->GetFrameSize(), camera->GetFrameWidth(),
                      camera->GetFrameHeight(), (v4l2_pix_fmt_t)camera->GetFrameFormat(), 20, &jpeg, &len) &&
        jpeg) {
        _server.sendBinary(fds, jpeg, len);
    }
    free(jpeg);
}

void home_link_task(void*)
{
    auto& hal  = GetHAL();
    auto& wifi = WifiManager::GetInstance();
    std::string token = load_or_create_token();
    bool enabled      = enabled_setting();
    uint32_t last_status_push = 0;
    uint32_t last_frame_push  = 0;
    uint8_t last_battery      = 255;
    bool network_kicked       = false;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(200));

        if (_config_dirty.exchange(false)) {
            token   = load_or_create_token();
            enabled = enabled_setting();
            _server.stop();
        }

        if (_pending_reboot >= 0) {
            const int target = _pending_reboot.exchange(-1);
            vTaskDelay(pdMS_TO_TICKS(300));
            hal.requestWarmReboot(target);
        }

        if (!network_kicked && !hal.getXiaozhiConfig().startAiAgentOnBoot && !wifi.IsConnected() &&
            !wifi.IsConfigMode() && hal.millis() > 8000 && !SsidManager::GetInstance().GetSsidList().empty()) {
            // Without saved credentials the board would open the App-setup flow and reboot; leave it alone.
            // Launcher does not bring Wi-Fi up by itself; Home Link needs it. Not gated on the
            // App-binding flag: a factory NVS may not carry it and binding is impossible on this fork.
            network_kicked        = true;
            _network_started_by_us = true;
            mclog::tagInfo(_tag, "starting network for home link");
            Board::GetInstance().StartNetwork();
        }

        const bool connected = wifi.IsConnected();
        if (connected && enabled && !_server.running()) {
            if (_server.start(kPort, token)) start_mdns();
        } else if ((!connected || !enabled) && _server.running()) {
            _server.stop();
        }
        if (!_server.running()) continue;

        const uint32_t now = hal.millis();
        if (now - last_status_push >= 5000) {
            last_status_push = now;
            if (_server.clientCount() > 0) {
                _server.broadcast("{\"type\":\"status\",\"status\":" + status_json() + "}");
            }
        }
        const uint8_t battery = hal.getBatteryLevel();
        if (battery != last_battery && last_battery != 255 && _server.clientCount() > 0) {
            ArduinoJson::JsonDocument doc;
            doc["type"]     = "battery";
            doc["level"]    = battery;
            doc["charging"] = hal.isBatteryCharging();
            std::string out;
            ArduinoJson::serializeJson(doc, out);
            _server.broadcast(out);
        }
        last_battery = battery;
        if (now - last_frame_push >= 200) {
            last_frame_push = now;
            push_camera_frames();
        }
    }
}

}  // namespace

void Hal::startHomeLink()
{
    if (_task_started.exchange(true)) return;
    mclog::tagInfo(_tag, "start");

    _server.setCommandHandler(handle_command);
    _server.setCameraToggleHandler([](int fd, bool enabled) {
        mclog::tagInfo(_tag, "camera frames {} for fd {}", enabled ? "on" : "off", fd);
    });

    onHeadPetGesture.connect([](HeadPetGesture gesture) {
        const char* name = gesture == HeadPetGesture::Press          ? "press"
                           : gesture == HeadPetGesture::Release      ? "release"
                           : gesture == HeadPetGesture::SwipeForward ? "swipe_forward"
                           : gesture == HeadPetGesture::SwipeBackward ? "swipe_backward"
                                                                      : "none";
        _server.broadcast(std::string("{\"type\":\"head_pet\",\"gesture\":\"") + name + "\"}");
    });
    onImuMotionEvent.connect([](ImuMotionEvent event) {
        const char* name = event == ImuMotionEvent::Shake ? "shake" : event == ImuMotionEvent::PickUp ? "pick_up" : "none";
        _server.broadcast(std::string("{\"type\":\"imu\",\"event\":\"") + name + "\"}");
    });

    xTaskCreatePinnedToCore(home_link_task, "home_link", 6144, nullptr, 2, nullptr, 0);
}

void Hal::stopHomeLink()
{
    _server.stop();
    if (_network_started_by_us.exchange(false)) {
        mclog::tagInfo(_tag, "stopping station started for home link");
        WifiManager::GetInstance().StopStation();
    }
}

bool Hal::isHomeLinkRunning()
{
    return _server.running();
}

HomeLinkInfo Hal::getHomeLinkInfo()
{
    HomeLinkInfo info;
    info.enabled = enabled_setting();
    info.running = _server.running();
    info.port    = kPort;
    info.token   = load_or_create_token();
    auto& wifi   = WifiManager::GetInstance();
    info.ip      = wifi.IsConnected() ? wifi.GetIpAddress() : "";
    return info;
}

void Hal::regenerateHomeLinkToken()
{
    Settings settings(_nvs_ns, true);
    settings.EraseKey(_nvs_token);
    _config_dirty = true;
    mclog::tagInfo(_tag, "token regenerated");
}

void Hal::setHomeLinkEnabled(bool enabled)
{
    Settings settings(_nvs_ns, true);
    settings.SetBool(_nvs_enable, enabled);
    _config_dirty = true;
}

void Hal::setCurrentAppName(std::string_view name)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(_app_mutex);
        changed      = _current_app != name;
        _current_app = std::string(name);
    }
    if (changed && _server.running()) {
        _server.broadcast("{\"type\":\"app\",\"name\":\"" + std::string(name) + "\"}");
    }
}

std::string Hal::getCurrentAppName()
{
    std::lock_guard<std::mutex> lock(_app_mutex);
    return _current_app;
}

void Hal::setHomeAppActive(bool active, std::function<void()> restoreDefaultModifiers)
{
    _home_active               = active;
    _restore_default_modifiers = active ? std::move(restoreDefaultModifiers) : nullptr;
}

bool Hal::isHomeAppActive()
{
    return _home_active.load();
}

uint32_t Hal::homeLinkLastMotionTick()
{
    return _last_motion_tick.load();
}

bool Hal::homeLinkStartedNetwork()
{
    return _network_started_by_us.load();
}
