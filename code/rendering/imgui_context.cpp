#include "imgui_context.hpp"
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <cstdio>

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

    const char* fonts_dir = IMGUI_FONTS_DIR;

    auto load = [&](const char* filename) -> ImFont*
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", fonts_dir, filename);
        ImFont* f = ImGui::GetIO().Fonts->AddFontFromFileTTF(path, base_size);
        IM_ASSERT(f != nullptr);
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
