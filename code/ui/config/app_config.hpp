#pragma once

#include "imgui.h"
#include <string>

struct app_config
{
    float clear_r = 0.45f;
    float clear_g = 0.55f;
    float clear_b = 0.60f;
    float clear_a = 1.00f;

    ImVec4 clear_color() const { return ImVec4(clear_r, clear_g, clear_b, clear_a); }
    void   set_clear_color(const ImVec4& c) { clear_r = c.x; clear_g = c.y; clear_b = c.z; clear_a = c.w; }
};

bool app_config_load(app_config& cfg, const std::string& path);
void app_config_save(const app_config& cfg, const std::string& path);
