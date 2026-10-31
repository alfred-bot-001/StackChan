// Host-side tests for the Home Link request parser (no ESP-IDF needed).
#include "hal/utils/home_link/home_link_protocol.h"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace home_link;

static int failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

static const std::string T = "0123456789abcdef0123456789abcdef";

static void test_bearer()
{
    CHECK(validate_bearer("Bearer " + T, T));
    CHECK(!validate_bearer("", T));
    CHECK(!validate_bearer("bearer " + T, T));
    CHECK(!validate_bearer("Bearer  " + T, T));
    std::string wrong = T;
    wrong[31]         = wrong[31] == 'f' ? 'e' : 'f';
    CHECK(!validate_bearer("Bearer " + wrong, T));
    CHECK(!validate_bearer("Bearer " + T.substr(0, 31), T));
    CHECK(!validate_bearer("Bearer " + T, ""));
}

static void test_routes()
{
    auto p = parse_request("GET", "/v1/status", "");
    CHECK(p.ok && p.command == Command::Status && p.http_status == 200);
    p = parse_request("GET", "/v1/status?x=1", "");
    CHECK(p.ok && p.command == Command::Status);
    p = parse_request("GET", "/v1/nothing", "");
    CHECK(!p.ok && p.http_status == 404 && p.error_code == "not_found");
    p = parse_request("GET", "/v1/motion", "");
    CHECK(!p.ok && p.http_status == 405);
    p = parse_request("DELETE", "/v1/modifiers", "");
    CHECK(p.ok && p.command == Command::ClearModifiers);
    p = parse_request("POST", "/v1/system/reboot", "");
    CHECK(p.ok && p.command == Command::Reboot && p.command_id.empty());
    p = parse_request("POST", "/v1/system/reboot", "{\"command_id\":\"cmd-00000001\"}");
    CHECK(p.ok && p.command_id == "cmd-00000001");
}

static void test_validation()
{
    auto p = parse_request("POST", "/v1/audio/volume", "{\"volume\":101}");
    CHECK(!p.ok && p.http_status == 400 && p.error_code == "bad_request");
    p = parse_request("POST", "/v1/audio/volume", "{\"volume\":40}");
    CHECK(p.ok && p.args.volume == 40);
    p = parse_request("POST", "/v1/audio/volume", "{}");
    CHECK(!p.ok);
    p = parse_request("POST", "/v1/motion", "{\"yaw\":30,\"speed\":300,\"command_id\":\"cmd-0001\"}");
    CHECK(p.ok && p.args.yaw == 30 && p.args.pitch == -9999 && p.args.speed == 300 && p.command_id == "cmd-0001");
    p = parse_request("POST", "/v1/motion", "{\"speed\":300}");
    CHECK(!p.ok);
    p = parse_request("POST", "/v1/motion", "{\"yaw\":129}");
    CHECK(!p.ok);
    p = parse_request("POST", "/v1/motion", "{\"yaw\":10,\"command_id\":\"bad id\"}");
    CHECK(!p.ok);
    p = parse_request("POST", "/v1/motion", "{\"yaw\":10,\"command_id\":\"short\"}");
    CHECK(!p.ok);
    p = parse_request("POST", "/v1/motion", "{\"yaw\":10");
    CHECK(!p.ok);
    std::string big(kMaxBodyBytes + 1, ' ');
    p = parse_request("POST", "/v1/motion", big);
    CHECK(!p.ok && p.http_status == 400);
    p = parse_request("POST", "/v1/led", "{\"leds\":[{\"index\":0,\"r\":10,\"g\":20,\"b\":30},{\"index\":11,\"r\":0,\"g\":0,\"b\":0}]}");
    CHECK(p.ok && p.args.leds.size() == 2 && p.args.leds[1].index == 11);
    p = parse_request("POST", "/v1/led", "{\"leds\":[{\"index\":12,\"r\":0,\"g\":0,\"b\":0}]}");
    CHECK(!p.ok);
    p = parse_request("POST", "/v1/led", "{\"r\":169,\"g\":0,\"b\":0}");
    CHECK(!p.ok);
    p = parse_request("POST", "/v1/led", "{\"r\":0,\"g\":0,\"b\":120}");
    CHECK(p.ok && p.args.b == 120 && p.args.leds.empty());
    p = parse_request("POST", "/v1/sound", "{\"name\":\"new_notification\"}");
    CHECK(p.ok && p.args.sound == "new_notification");
    p = parse_request("POST", "/v1/sound", "{\"name\":\"../x\"}");
    CHECK(!p.ok);
    p = parse_request("POST", "/v1/avatar/emotion", "{\"emotion\":\"happy\"}");
    CHECK(p.ok && p.args.emotion == "happy" && p.args.duration_ms == 3000);
    p = parse_request("POST", "/v1/avatar/emotion", "{\"emotion\":\"laughing\"}");
    CHECK(!p.ok);
    p = parse_request("POST", "/v1/avatar/speech", "{\"text\":\"hi\"}");
    CHECK(p.ok && p.args.text == "hi" && p.args.duration_ms == 6000);
    p = parse_request("POST", "/v1/avatar/speech", "{\"text\":\"\"}");
    CHECK(!p.ok);
    p = parse_request("POST", "/v1/motion/look", "{\"x\":0.5,\"y\":-1}");
    CHECK(p.ok && p.args.has_xy && p.args.x > 0.49f && p.args.y < -0.99f && p.args.speed == 500);
    p = parse_request("POST", "/v1/motion/look", "{\"x\":1.5,\"y\":0}");
    CHECK(!p.ok);
    p = parse_request("POST", "/v1/motion/torque", "{\"enabled\":false}");
    CHECK(p.ok && p.args.torque == 0);
    p = parse_request("POST", "/v1/motion/torque", "{\"enabled\":1}");
    CHECK(!p.ok);
    p = parse_request("POST", "/v1/avatar", "{\"mouth\":{\"size\":1}}");
    CHECK(p.ok && p.args.raw_json.find("mouth") != std::string::npos);
    p = parse_request("POST", "/v1/avatar", "{\"foo\":1}");
    CHECK(!p.ok);
    p = parse_request("POST", "/v1/dance", "[{\"durationMs\":500,\"yawServo\":{\"angle\":30}}]");
    CHECK(p.ok && p.args.raw_json[0] == '[');
    p = parse_request("POST", "/v1/dance", "[]");
    CHECK(!p.ok);
    p = parse_request("POST", "/v1/dance", "{\"sequence\":[]}");
    CHECK(!p.ok);
    p = parse_request("POST", "/v1/neon", "{\"leftRgbColor\":\"#ff0000\"}");
    CHECK(p.ok);
}

static void test_misc()
{
    CHECK(!is_home_only(Command::Led));
    CHECK(!is_home_only(Command::Volume));
    CHECK(is_home_only(Command::Motion));
    CHECK(is_home_only(Command::Snapshot));
    CHECK(result_json("cmd-0001", "accepted") == "{\"command_id\":\"cmd-0001\",\"state\":\"accepted\"}");
    CHECK(result_json("", "completed") == "{\"state\":\"completed\"}");
    CHECK(error_json("unauthorized", "bad token") == "{\"error\":\"unauthorized\",\"message\":\"bad token\"}");
    CHECK(error_json_with_app("home_app_not_active", "m", "AI.AGENT").find("\"current_app\":\"AI.AGENT\"") != std::string::npos);
    const std::string busy = error_json_conversation("AI.AGENT", "speaking");
    CHECK(busy.find("\"error\":\"conversation_active\"") != std::string::npos);
    CHECK(busy.find("\"current_app\":\"AI.AGENT\"") != std::string::npos);
    CHECK(busy.find("\"conversation\":\"speaking\"") != std::string::npos);
    const uint8_t bytes[4] = {0x00, 0xab, 0xff, 0x10};
    CHECK(make_token_hex(bytes, 4) == "00abff10");
    CHECK(valid_command_id("console-abcdef0123456789"));
    CHECK(!valid_command_id("-abcdefgh"));
}

int main()
{
    test_bearer();
    test_routes();
    test_validation();
    test_misc();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("home_link_protocol_test: all passed\n");
    return 0;
}
