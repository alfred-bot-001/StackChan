/*
 * SPDX-FileCopyrightText: 2026 alfred-bot-001 (home-link fork)
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <smooth_lvgl.hpp>
#include <uitk/short_namespace.hpp>
#include <memory>

/**
 * @brief HOME app: shows the default avatar and lets the family console drive
 * expression, motion, neon lights and camera through Home Link.
 */
class AppHomeLink : public mooncake::AppAbility {
public:
    AppHomeLink();
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

    static void addDefaultModifiers();

private:
    std::unique_ptr<uitk::lvgl_cpp::Label> _hint;
    uint32_t _last_hint_update = 0;
};
