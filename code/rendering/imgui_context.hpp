#pragma once

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "vulkan_context.hpp"
#include <SDL3/SDL.h>

class imgui_context {
public:
    imgui_context();
    void init(SDL_Window *window, vulkan_context &vk, ImGui_ImplVulkanH_Window *wd, float main_scale);
    void load_fonts(float main_scale);
    void shutdown();
    void new_frame();
    void render(ImGui_ImplVulkanH_Window *wd, vulkan_context &vk, const ImVec4 &clear_color);

    ImFont *font_cousine;
    ImFont *font_droid_sans;
    ImFont *font_karla;
    ImFont *font_proggy_clean;
    ImFont *font_proggy_tiny;
    ImFont *font_roboto;
};
