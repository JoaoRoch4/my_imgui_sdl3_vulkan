#include "pch.hpp"

#include "app.hpp"


#include "app_context.hpp"
#include "app_coordinator.hpp"
#include "fps_plot.hpp"
#include "imgui_context.hpp"
#include "sdl3_context.hpp"
#include "style_editor.hpp"
#include "vulkan_context.hpp"
#include "window_fullscreen_utils.hpp"
#include "window_state_toml.hpp"



App::App()
    : m_AppContext(nullptr)
    , m_Sdl(nullptr) {

    m_AppContext = AppContext::GetInstance();
}

void App::KickStart() {



 }


bool App::run() {
    const auto app_start_time = std::chrono::steady_clock::now();

    sdl3_context sdl;
    if (!sdl.init("Dear ImGui SDL3+Vulkan example", 1280, 800))
	return false;

    vulkan_context vk;
    {
	std::vector<const char*> extensions;
	uint32_t		 count = 0;
	const char* const*	 exts  = SDL_Vulkan_GetInstanceExtensions(&count);
	extensions.reserve(count);
	for (uint32_t i = 0; i < count; i++)
	    extensions.push_back(exts[i]);
	vk.setup(extensions);
    }

    VkSurfaceKHR surface;
    if (SDL_Vulkan_CreateSurface(sdl.window, vk.instance, vk.allocator, &surface) == 0) {
	std::printf("Failed to create Vulkan surface.\n");
	return 1;
    }

    int w;
    int h;
    SDL_GetWindowSize(sdl.window, &w, &h);
    ImGui_ImplVulkanH_Window* wd = &vk.main_window_data;
    vk.setup_window(wd, surface, w, h);
    SDL_SetWindowPosition(sdl.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(sdl.window);

    imgui_context imgui;
    imgui.init(sdl.window, vk, wd, sdl.main_scale);
    ImPlot::CreateContext();

    FpsPlot fps_plot;

    StyleEditor style_editor;
    style_editor.InitDefaults();
    style_editor.SetFonts({
	{"Cousine", imgui.font_cousine},
	{"DroidSans", imgui.font_droid_sans},
	{"Karla", imgui.font_karla},
	{"ProggyClean", imgui.font_proggy_clean},
	{"ProggyTiny", imgui.font_proggy_tiny},
	{"Roboto", imgui.font_roboto},
    });

    // Shared cache folder at the project root — deliberately OUTSIDE build/debug,
    // build/release and build/release-log so all three builds read & write the
    // SAME window_state.toml (plus thumbnails and video cache). SDL_GetBasePath()
    // is the executable dir (e.g. <root>/build/debug/); two parents up is <root>.
    const auto exe_dir	    = std::filesystem::path(SDL_GetBasePath());
    const auto project_root = exe_dir.parent_path().parent_path();
    const auto cache_dir    = project_root / "cache";
    std::filesystem::create_directories(cache_dir);

    static const std::string state_path = (cache_dir / "window_state.toml").string();
    WindowStateToml	     state;
    LoadWindowStateToml(state_path, state);
    style_editor.ApplyLayout(state);

    bool show_demo_window    = state.show_demo_window;
    bool show_another_window = state.show_another_window;
    bool vsync		     = state.vsync;
    vk.set_vsync(wd, vsync);

    AppCoordinator menu_bar;
    menu_bar.Setup(&style_editor, sdl.window, &vk, &show_demo_window, &show_another_window, [&](bool enabled) {
	vsync = enabled;
	vk.set_vsync(wd, vsync);
    });
    menu_bar.LoadOpenedFilesHistoryFromToml(state_path);
    menu_bar.SetStatePath(state_path);
    menu_bar.ApplyHistory(state);
    menu_bar.ApplyRuntimeConfig(state);

    menu_bar.SetThumbDir(cache_dir / "thumbs");
    menu_bar.SetDownloadCacheDir(cache_dir / "video_cache");
    ImVec4 clear_color = state.clear_color
	? ImVec4(static_cast<float>(state.clear_color->r) / 255.0f, static_cast<float>(state.clear_color->g) / 255.0f,
	      static_cast<float>(state.clear_color->b) / 255.0f, static_cast<float>(state.clear_color->a) / 255.0f)
	: ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    bool  done	      = false;
    bool  is_dragging = false;
    float drag_start_x;
    float drag_start_y;

    while (!done) {
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
	    menu_bar.HandleSdlEvent(event);
	    ImGui_ImplSDL3_ProcessEvent(&event);
	    ImGuiIO& io = ImGui::GetIO();
	    (void)io;

	    if (event.type == SDL_EVENT_QUIT)
		done = true;
	    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(sdl.window))
		done = true;

	    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
		if (event.button.button == SDL_BUTTON_LEFT && (SDL_GetModState() & SDL_KMOD_SHIFT)) {
		    is_dragging = true;
		    SDL_GetGlobalMouseState(&drag_start_x, &drag_start_y);

		    // Force ImGui to release active widgets so window dragging can take over.
		    ImGui::ClearActiveID();
		}
	    }

	    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
		if (event.button.button == SDL_BUTTON_LEFT)
		    is_dragging = false;
	    }

	    if (event.type == SDL_EVENT_MOUSE_MOTION && is_dragging) {
		float current_x;
		float current_y;
		SDL_GetGlobalMouseState(&current_x, &current_y);

		float delta_x = current_x - drag_start_x;
		float delta_y = current_y - drag_start_y;

		int win_x;
		int win_y;
		SDL_GetWindowPosition(sdl.window, &win_x, &win_y);

		SDL_SetWindowPosition(sdl.window, win_x + static_cast<int>(delta_x), win_y + static_cast<int>(delta_y));

		drag_start_x = current_x;
		drag_start_y = current_y;
	    }

	    if (event.type == SDL_EVENT_KEY_DOWN) {
		if (event.key.key == SDLK_F11) {
		    toggle_window_fullscreen(sdl.window);
		}
	    }
	}

	done |= menu_bar.request_quit;

	if (SDL_GetWindowFlags(sdl.window) & SDL_WINDOW_MINIMIZED) {
	    SDL_Delay(10);
	    continue;
	}

	int fb_width;
	int fb_height;
	SDL_GetWindowSize(sdl.window, &fb_width, &fb_height);
	if (fb_width > 0 && fb_height > 0 && (vk.swap_chain_rebuild || wd->Width != fb_width || wd->Height != fb_height))
	    vk.resize_window(wd, fb_width, fb_height);

	imgui.new_frame();

	const auto   uptime_now		  = std::chrono::steady_clock::now();
	const double uptime_seconds	  = std::chrono::duration<double>(uptime_now - app_start_time).count();
	const auto   uptime_total_seconds = static_cast<int>(uptime_seconds);
	const int    uptime_hours	  = uptime_total_seconds / 3600;
	const int    uptime_minutes	  = (uptime_total_seconds % 3600) / 60;
	const int    uptime_secs	  = uptime_total_seconds % 60;

	fps_plot.add_sample(ImGui::GetIO().Framerate);

	menu_bar.Build();
	fps_plot.draw(uptime_seconds);

	{
	    const ImGuiViewport* vp = ImGui::GetMainViewport();
	    ImGui::SetNextWindowPos(vp->WorkPos);
	    ImGui::SetNextWindowSize(vp->WorkSize);
	    ImGui::SetNextWindowViewport(vp->ID);
	    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	    constexpr ImGuiWindowFlags dock_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
		| ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus
		| ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground;
	    ImGui::Begin("UI", nullptr, dock_flags);
	    ImGui::PopStyleVar(3);
	    ImGui::DockSpace(ImGui::GetID("UI"), ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
	    ImGui::End();
	}

	style_editor.Draw();

	if (show_demo_window)
	    ImGui::ShowDemoWindow(&show_demo_window);

	{
	    static float f	 = 0.0f;
	    static int	 counter = 0;

	    if (state.hello_world_window.valid) {
		ImGui::SetNextWindowPos({state.hello_world_window.x, state.hello_world_window.y}, ImGuiCond_Once);
		ImGui::SetNextWindowSize({state.hello_world_window.w, state.hello_world_window.h}, ImGuiCond_Once);
	    }
	    ImGui::Begin("Hello, world!");
	    {
		ImVec2 pos		 = ImGui::GetWindowPos();
		ImVec2 size		 = ImGui::GetWindowSize();
		state.hello_world_window = {true, pos.x, pos.y, size.x, size.y};
	    }
	    ImGui::Text("This is some useful text.");
	    ImGui::Checkbox("Demo Window", &show_demo_window);
	    ImGui::Checkbox("Another Window", &show_another_window);
	    ImGui::Checkbox("Style Editor", &style_editor.IsOpen);

	    if (ImGui::Checkbox("VSync", &vsync))
		vk.set_vsync(wd, vsync);
	    ImGui::SliderFloat("float", &f, 0.0f, 1.0f);
	    ImGui::ColorEdit3("clear color", &clear_color.x);
	    if (ImGui::Button("Button"))
		counter++;
	    ImGui::SameLine();
	    ImGui::Text("counter = %d", counter);
	    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate,
		ImGui::GetIO().Framerate);
	    ImGui::Text("Uptime: %02d:%02d:%02d", uptime_hours, uptime_minutes, uptime_secs);
	    ImGui::End();
	}

	if (show_another_window) {
	    if (state.another_window.valid) {
		ImGui::SetNextWindowPos({state.another_window.x, state.another_window.y}, ImGuiCond_Once);
		ImGui::SetNextWindowSize({state.another_window.w, state.another_window.h}, ImGuiCond_Once);
	    }
	    ImGui::Begin("Another Window", &show_another_window);
	    {
		ImVec2 pos	     = ImGui::GetWindowPos();
		ImVec2 size	     = ImGui::GetWindowSize();
		state.another_window = {true, pos.x, pos.y, size.x, size.y};
	    }
	    ImGui::Text("Hello from another window!");
	    if (ImGui::Button("Close Me"))
		show_another_window = false;
	    ImGui::End();
	}

	imgui.render(wd, vk, clear_color);
    }

    const bool reopen_requested = menu_bar.request_reopen;

    vkDeviceWaitIdle(vk.device);
    menu_bar.Shutdown();
    state.show_demo_window    = show_demo_window;
    state.show_another_window = show_another_window;
    state.vsync		      = vsync;
    state.clear_color = WindowStateToml::ColorToml {static_cast<int>(std::lround(clear_color.x * 255.0f + 0.5f)),
	static_cast<int>(std::lround(clear_color.y * 255.0f + 0.5f)),
	static_cast<int>(std::lround(clear_color.z * 255.0f + 0.5f)),
	static_cast<int>(std::lround(clear_color.w * 255.0f + 0.5f))};
    style_editor.ExportLayout(&state);
    menu_bar.ExportHistory(&state);
    menu_bar.ExportRuntimeConfig(&state);
    SaveWindowStateToml(state_path, state);
    ImPlot::DestroyContext();
    imgui.shutdown();
    vk.cleanup_window(wd);
    vk.cleanup();
    sdl.shutdown();

    return reopen_requested ? App::k_reopen_exit_code : 0;
}
