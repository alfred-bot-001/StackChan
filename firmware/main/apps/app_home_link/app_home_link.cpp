/*
 * SPDX-FileCopyrightText: 2026 alfred-bot-001 (home-link fork)
 * SPDX-License-Identifier: MIT
 */
#include "app_home_link.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <stackchan/stackchan.h>
#include <stackchan/modifiers/modifiers.h>
#include <apps/common/common.h>
#include <fmt/format.h>

using namespace mooncake;
using namespace stackchan;
using namespace smooth_ui_toolkit::lvgl_cpp;

static constexpr uint32_t kThemeColor = 0x2E86AB;
static constexpr int kHomeAppIndex    = 7;

AppHomeLink::AppHomeLink()
{
    setAppInfo().name = "HOME";
    static auto icon  = assets::get_image("icon_sentinel.bin");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = kThemeColor;
    setAppInfo().userData       = (void*)&theme_color;
}

void AppHomeLink::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppHomeLink::addDefaultModifiers()
{
    auto& sc = GetStackChan();
    sc.addModifier(std::make_unique<BreathModifier>());
    sc.addModifier(std::make_unique<BlinkModifier>());
    sc.addModifier(std::make_unique<HeadPetModifier>());
    sc.addModifier(std::make_unique<IdleExpressionModifier>());
    sc.addModifier(std::make_unique<IdleMotionModifier>(8000, 12000));
}

void AppHomeLink::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    LvglLockGuard lock;

    auto avatar = std::make_unique<avatar::DefaultAvatar>();
    avatar->init(lv_screen_active());
    GetStackChan().attachAvatar(std::move(avatar));
    addDefaultModifiers();

    auto& motion = GetStackChan().motion();
    motion.setAutoAngleSyncEnabled(true);
    motion.setAutoTorqueReleaseEnabled(true);

    _hint = std::make_unique<Label>(lv_screen_active());
    _hint->setTextFont(&lv_font_montserrat_14);
    _hint->setTextColor(lv_color_hex(0x8BB8C9));
    _hint->align(LV_ALIGN_BOTTOM_LEFT, 8, -6);
    _hint->setText("HOME LINK");

    GetHAL().setHomeAppActive(true, []() { AppHomeLink::addDefaultModifiers(); });

    view::create_home_indicator([&]() { close(); }, 0x8BB8C9, 0x0E2F3D);
    view::create_status_bar(0x8BB8C9, 0x0E2F3D);
}

void AppHomeLink::onRunning()
{
    LvglLockGuard lock;

    auto& hal = GetHAL();
    if (hal.millis() - hal.homeLinkLastMotionTick() > 2000) {
        GetStackChan().motion().setAutoAngleSyncEnabled(true);
    }

    if (hal.millis() - _last_hint_update > 2000 && _hint) {
        _last_hint_update = hal.millis();
        auto info         = hal.getHomeLinkInfo();
        if (!info.enabled) {
            _hint->setText("HOME LINK disabled (Setup)");
        } else if (info.running && !info.ip.empty()) {
            _hint->setText(fmt::format("HOME LINK {}:{}", info.ip, info.port).c_str());
        } else {
            _hint->setText("HOME LINK waiting for Wi-Fi");
        }
    }

    GetStackChan().update();

    view::update_home_indicator();
    view::update_status_bar();
}

void AppHomeLink::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    GetHAL().setHomeAppActive(false, nullptr);

    {
        LvglLockGuard lock;
        GetStackChan().clearModifiers();
        GetStackChan().resetAvatar();
        _hint.reset();
        view::destroy_home_indicator();
        view::destroy_status_bar();
    }

    GetHAL().requestWarmReboot(kHomeAppIndex);
}
