#include "pch.hpp"

#include "imgui_context.hpp"
#include <imgui_impl_sdl3.h>

imgui_context::imgui_context()
    : font_cousine(nullptr)
    , font_droid_sans(nullptr)
    , font_karla(nullptr)
    , font_proggy_clean(nullptr)
    , font_proggy_tiny(nullptr)
    , font_roboto(nullptr)
{}
void imgui_context::init(SDL_Window* window, vulkan_context& vk, ImGui_ImplVulkanH_Window* wd, float main_scale)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplSDL3_InitForVulkan(window);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance       = vk.instance;
    init_info.PhysicalDevice = vk.physical_device;
    init_info.Device         = vk.device;
    init_info.QueueFamily    = vk.queue_family;
    init_info.Queue          = vk.queue;
    init_info.PipelineCache  = vk.pipeline_cache;
    init_info.DescriptorPool = vk.descriptor_pool;
    init_info.MinImageCount  = vk.min_image_count;
    init_info.ImageCount     = wd->ImageCount;
    init_info.Allocator      = vk.allocator;
    init_info.PipelineInfoMain.RenderPass   = wd->RenderPass;
    init_info.PipelineInfoMain.Subpass      = 0;
    init_info.PipelineInfoMain.MSAASamples  = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = vulkan_context::check_result;
    ImGui_ImplVulkan_Init(&init_info);

    load_fonts(main_scale);
}

void imgui_context::load_fonts(float main_scale)
{
    const float base_size = 16.0f * main_scale;
    ImFontConfig cfg;
    cfg.SizePixels = base_size;

    static const ImWchar k_symbol_ranges[] = {
        0x2000, 0x206F, // General Punctuation
        0x20A0, 0x20CF, // Currency Symbols
        0x2100, 0x214F, // Letterlike Symbols
        0x2190, 0x21FF, // Arrows
        0x2200, 0x22FF, // Mathematical Operators
        0x2300, 0x23FF, // Misc Technical
        0x2460, 0x24FF, // Enclosed Alphanumerics
        0x2500, 0x257F, // Box Drawing
        0x2580, 0x259F, // Block Elements
        0x25A0, 0x25FF, // Geometric Shapes
        0x2600, 0x26FF, // Misc Symbols
        0x2700, 0x27BF, // Dingbats
        0x27F0, 0x27FF, // Supplemental Arrows-A
        0,
    };

    static const ImWchar k_emoji_ranges[] = {
        0x1F300, 0x1F5FF, // Misc Symbols and Pictographs
        0x1F600, 0x1F64F, // Emoticons
        0,
    };

    const std::array<const char *, 4> symbol_candidates = {
        "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };

    const std::array<const char *, 4> emoji_candidates = {
        "/usr/share/fonts/truetype/noto/NotoEmoji-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoEmoji-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/noto/NotoEmoji-Regular.ttf",
    };

    const char* fonts_dir = IMGUI_FONTS_DIR;

    auto merge_fallback = [&](const std::array<const char *, 4> &candidates,
                              const ImWchar *ranges,
                              float size_px) {
        for (const char *candidate : candidates) {
            std::error_code ec;
            if (!std::filesystem::exists(candidate, ec))
                continue;

            ImFontConfig merge_cfg;
            merge_cfg.MergeMode = true;
            merge_cfg.PixelSnapH = true;
            merge_cfg.SizePixels = size_px;
            if (ImGui::GetIO().Fonts->AddFontFromFileTTF(candidate, size_px, &merge_cfg, ranges))
                return true;
        }
        return false;
    };

    auto load = [&](const char* filename) -> ImFont*
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", fonts_dir, filename);
        ImFont* f = ImGui::GetIO().Fonts->AddFontFromFileTTF(path, base_size);
        IM_ASSERT(f != nullptr);
        merge_fallback(symbol_candidates, k_symbol_ranges, base_size);
        merge_fallback(emoji_candidates, k_emoji_ranges, base_size);
        return f;
    };

    font_cousine      = load("Cousine-Regular.ttf");
    font_droid_sans   = load("DroidSans.ttf");
    font_karla        = load("Karla-Regular.ttf");
    font_proggy_clean = load("ProggyClean.ttf");
    font_proggy_tiny  = load("ProggyTiny.ttf");
    font_roboto       = load("Roboto-Medium.ttf");
}

void imgui_context::shutdown()
{
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void imgui_context::new_frame()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void imgui_context::render(ImGui_ImplVulkanH_Window* wd, vulkan_context& vk, const ImVec4& clear_color)
{
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
    if (!is_minimized)
    {
        vk.frame_render(wd, draw_data, clear_color);
        vk.frame_present(wd);
    }
}
