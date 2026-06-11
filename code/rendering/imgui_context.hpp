#pragma once

#include "pch.hpp"

#include "vulkan_context.hpp"

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

private:
    // Raw bytes of every merged fallback face (symbols / math / emoji / CJK / Hebrew /
    // Arabic / hieroglyphs). Each file is read exactly once and shared across all base
    // fonts via AddFontFromMemoryTTF(FontDataOwnedByAtlas=false), so a large face like
    // Noto Sans CJK isn't duplicated per base font. Must outlive the ImGui font atlas.
    std::vector<std::vector<std::byte>> fallback_font_blobs_;
};
