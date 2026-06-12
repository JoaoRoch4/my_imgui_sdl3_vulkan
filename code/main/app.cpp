#include "pch.hpp"

#include "app.hpp"


#include "app_context.hpp"
#include "image_job_system.hpp"
#include "window_fullscreen_utils.hpp"



App::App(StartupOptions opts)
	: m_Opts(std::move(opts)) {
	m_AppContext = AppContext::GetInstance();
}


bool App::KickStart() {
	// Name the main OS thread so it shows as "MainThread" instead of the process
	// name ("example_sdl3_vu") in the thread reflection panel and debuggers.
	pthread_setname_np(pthread_self(), "MainThread");

	m_AppStartTime = std::chrono::steady_clock::now();

	// Start the parallel image engine (decode/resize/encode worker pool wired to
	// ThreadOverwatch) for the whole session; shut it down in destroy().
	img::ImageJobSystem::instance().start();

	if (!m_Sdl.init("Dear ImGui SDL3+Vulkan example", 1280, 800))
		return false;

	{
		std::vector<char const*> extensions;
		uint32_t                 count = 0;
		char const* const*       exts  = SDL_Vulkan_GetInstanceExtensions(&count);
		extensions.reserve(count);
		for (uint32_t i = 0; i < count; i++)
			extensions.push_back(exts[i]);
		m_Vk.setup(extensions);
	}

	if (SDL_Vulkan_CreateSurface(m_Sdl.window, m_Vk.instance, m_Vk.allocator, &m_Surface) == 0) {
		std::printf("Failed to create Vulkan surface.\n");
		return false;
	}

	int w;
	int h;
	SDL_GetWindowSize(m_Sdl.window, &w, &h);
	m_Wd = &m_Vk.main_window_data;
	m_Vk.setup_window(m_Wd, m_Surface, w, h);
	SDL_SetWindowPosition(m_Sdl.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	SDL_ShowWindow(m_Sdl.window);

	m_Imgui.init(m_Sdl.window, m_Vk, m_Wd, m_Sdl.main_scale);
	ImPlot::CreateContext();

#ifdef _DEBUG
	// Debug builds: spawn a session-lifetime demo thread so the reflection panel
	// has sample content when it is shown. The panel itself is no longer
	// auto-opened — it appears only when --monitor-thread is passed (see below).
	// The thread stops + joins when this unique_ptr is destroyed.
	m_DebugDemoThread = spawn_demo_thread(std::chrono::seconds {0});
#endif

	m_StyleEditor.InitDefaults();
	// Google Noto bases first (the mains); add only the families that resolved at
	// runtime, then Dear ImGui's bundled faces as alternatives.
	std::vector<std::pair<std::string, ImFont *>> fonts;
	if (m_Imgui.font_noto_sans)  fonts.emplace_back("Noto Sans", m_Imgui.font_noto_sans);
	if (m_Imgui.font_noto_mono)  fonts.emplace_back("Noto Sans Mono", m_Imgui.font_noto_mono);
	if (m_Imgui.font_noto_serif) fonts.emplace_back("Noto Serif", m_Imgui.font_noto_serif);
	fonts.emplace_back("Cousine", m_Imgui.font_cousine);
	fonts.emplace_back("DroidSans", m_Imgui.font_droid_sans);
	fonts.emplace_back("Karla", m_Imgui.font_karla);
	fonts.emplace_back("ProggyClean", m_Imgui.font_proggy_clean);
	fonts.emplace_back("ProggyTiny", m_Imgui.font_proggy_tiny);
	fonts.emplace_back("Roboto", m_Imgui.font_roboto);
	m_StyleEditor.SetFonts(std::move(fonts));

	// Shared cache folder at the project root — deliberately OUTSIDE build/debug,
	// build/release and build/release-log so all three builds read & write the
	// SAME window_state.toml (plus thumbnails and video cache). SDL_GetBasePath()
	// is the executable dir (e.g. <root>/build/debug/); two parents up is <root>.
	auto const exe_dir      = std::filesystem::path(SDL_GetBasePath());
	auto const project_root = exe_dir.parent_path().parent_path();
	auto const cache_dir    = project_root / "cache";
	std::filesystem::create_directories(cache_dir);

	m_StatePath = (cache_dir / "window_state.toml").string();
	LoadWindowStateToml(m_StatePath, m_State);

	// ---- Apply command-line overrides on top of the loaded TOML -------------
	// Snapshot the persisted file-explorer visibility BEFORE overriding it, so
	// destroy() can restore it and keep the CLI override session-only.
	m_OriginalShowFileExplorer = m_State.show_file_explorer_window;
	if (m_Opts.file_browser)
		m_State.show_file_explorer_window = *m_Opts.file_browser;

	// --monitor-thread: force the Threads reflection panel open (release too).
	if (m_Opts.monitor_thread)
		m_ShowThreadPanel = true;

	m_StyleEditor.ApplyLayout(m_State);

	m_ShowDemoWindow    = m_State.show_demo_window;
	m_ShowAnotherWindow = m_State.show_another_window;
	m_Vsync             = m_State.vsync;
	m_Vk.set_vsync(m_Wd, m_Vsync);

	m_MenuBar.Setup(&m_StyleEditor, m_Sdl.window, &m_Vk, &m_ShowDemoWindow, &m_ShowAnotherWindow, [this](bool enabled) {
		m_Vsync = enabled;
		m_Vk.set_vsync(m_Wd, m_Vsync);
	});
	m_MenuBar.LoadOpenedFilesHistoryFromToml(m_StatePath);
	m_MenuBar.SetStatePath(m_StatePath);
	m_MenuBar.ApplyHistory(m_State);
	m_MenuBar.ApplyRuntimeConfig(m_State);

	// --no-video / --no-media: gate the media-routing choke point. Video is
	// disabled by either flag; images only by --no-media. Subsystems still boot;
	// they just receive nothing to load.
	m_MenuBar.SetMediaPolicy(/*allow_video=*/!(m_Opts.disable_video || m_Opts.disable_media),
		/*allow_image=*/!m_Opts.disable_media);

	// Echo what the flags did to BOTH stdout and the integrated console, then
	// auto-open the console so the feedback is visible. With no flags the list is
	// empty: nothing prints, the console stays as the TOML left it. This runs
	// AFTER ApplyRuntimeConfig (which sets console visibility from TOML) so the
	// auto-open is not clobbered.
	if (auto const args_feedback = describe_startup_options(m_Opts); !args_feedback.empty()) {
		for (auto const &line : args_feedback) {
			std::println("[Args] {}", line);
			m_MenuBar.ConsoleLog(std::format("[Args] {}", line));
		}
		m_MenuBar.ShowConsole();
	}

	m_MenuBar.SetThumbDir(cache_dir / "thumbs");
	m_MenuBar.SetDownloadCacheDir(cache_dir / "video_cache");

	m_ClearColor = m_State.clear_color
		? ImVec4(static_cast<float>(m_State.clear_color->r) / 255.0f, static_cast<float>(m_State.clear_color->g) / 255.0f,
			  static_cast<float>(m_State.clear_color->b) / 255.0f, static_cast<float>(m_State.clear_color->a) / 255.0f)
		: ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

	return true;
}


void App::tick() {
	while (!m_Done) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			m_MenuBar.HandleSdlEvent(event);
			ImGui_ImplSDL3_ProcessEvent(&event);
			ImGuiIO& io = ImGui::GetIO();
			(void)io;

			if (event.type == SDL_EVENT_QUIT)
				m_Done = true;
			if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(m_Sdl.window))
				m_Done = true;

			if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
				if (event.button.button == SDL_BUTTON_LEFT && (SDL_GetModState() & SDL_KMOD_SHIFT)) {
					m_IsDragging = true;
					SDL_GetGlobalMouseState(&m_DragStartX, &m_DragStartY);

					// Force ImGui to release active widgets so window dragging can take over.
					ImGui::ClearActiveID();
				}
			}

			if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
				if (event.button.button == SDL_BUTTON_LEFT)
					m_IsDragging = false;
			}

			if (event.type == SDL_EVENT_MOUSE_MOTION && m_IsDragging) {
				float current_x;
				float current_y;
				SDL_GetGlobalMouseState(&current_x, &current_y);

				float delta_x = current_x - m_DragStartX;
				float delta_y = current_y - m_DragStartY;

				int win_x;
				int win_y;
				SDL_GetWindowPosition(m_Sdl.window, &win_x, &win_y);

				SDL_SetWindowPosition(m_Sdl.window, win_x + static_cast<int>(delta_x), win_y + static_cast<int>(delta_y));

				m_DragStartX = current_x;
				m_DragStartY = current_y;
			}

			if (event.type == SDL_EVENT_KEY_DOWN) {
				if (event.key.key == SDLK_F11) {
					toggle_window_fullscreen(m_Sdl.window);
				}
			}
		}

		m_Done |= m_MenuBar.request_quit;

		if (SDL_GetWindowFlags(m_Sdl.window) & SDL_WINDOW_MINIMIZED) {
			SDL_Delay(10);
			continue;
		}

		int fb_width;
		int fb_height;
		SDL_GetWindowSize(m_Sdl.window, &fb_width, &fb_height);
		if (fb_width > 0 && fb_height > 0
			&& (m_Vk.swap_chain_rebuild || m_Wd->Width != fb_width || m_Wd->Height != fb_height))
			m_Vk.resize_window(m_Wd, fb_width, fb_height);

		m_Imgui.new_frame();

		auto const   uptime_now           = std::chrono::steady_clock::now();
		double const uptime_seconds       = std::chrono::duration<double>(uptime_now - m_AppStartTime).count();
		auto const   uptime_total_seconds = static_cast<int>(uptime_seconds);
		int const    uptime_hours         = uptime_total_seconds / 3600;
		int const    uptime_minutes       = (uptime_total_seconds % 3600) / 60;
		int const    uptime_secs          = uptime_total_seconds % 60;

		m_FpsPlot.add_sample(ImGui::GetIO().Framerate);

		m_MenuBar.Build();
		m_FpsPlot.draw(uptime_seconds);
		m_ThreadPanel.draw(&m_ShowThreadPanel);

		{
			ImGuiViewport const* vp = ImGui::GetMainViewport();
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

		m_StyleEditor.Draw();

		if (m_ShowDemoWindow)
			ImGui::ShowDemoWindow(&m_ShowDemoWindow);

		{
			static float f       = 0.0f;
			static int   counter = 0;

			if (m_State.hello_world_window.valid) {
				ImGui::SetNextWindowPos({m_State.hello_world_window.x, m_State.hello_world_window.y}, ImGuiCond_Once);
				ImGui::SetNextWindowSize({m_State.hello_world_window.w, m_State.hello_world_window.h}, ImGuiCond_Once);
			}
			ImGui::Begin("Hello, world!");
			{
				ImVec2 pos                 = ImGui::GetWindowPos();
				ImVec2 size                = ImGui::GetWindowSize();
				m_State.hello_world_window = {true, pos.x, pos.y, size.x, size.y};
			}
			ImGui::Text("This is some useful text.");
			ImGui::Checkbox("Demo Window", &m_ShowDemoWindow);
			ImGui::Checkbox("Another Window", &m_ShowAnotherWindow);
			ImGui::Checkbox("Style Editor", &m_StyleEditor.IsOpen);
			ImGui::Checkbox("String Test", &m_StrTest);

			if (ImGui::Checkbox("VSync", &m_Vsync))
				m_Vk.set_vsync(m_Wd, m_Vsync);
			ImGui::SliderFloat("float", &f, 0.0f, 1.0f);
			ImGui::ColorEdit3("clear color", &m_ClearColor.x);
			if (ImGui::Button("Button"))
				counter++;
			ImGui::SameLine();
			ImGui::Text("counter = %d", counter);
			ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate,
				ImGui::GetIO().Framerate);
			ImGui::Text("Uptime: %02d:%02d:%02d", uptime_hours, uptime_minutes, uptime_secs);
			ImGui::End();
		}

		if (m_ShowAnotherWindow) {
			if (m_State.another_window.valid) {
				ImGui::SetNextWindowPos({m_State.another_window.x, m_State.another_window.y}, ImGuiCond_Once);
				ImGui::SetNextWindowSize({m_State.another_window.w, m_State.another_window.h}, ImGuiCond_Once);
			}
			ImGui::Begin("Another Window", &m_ShowAnotherWindow);
			{
				ImVec2 pos             = ImGui::GetWindowPos();
				ImVec2 size            = ImGui::GetWindowSize();
				m_State.another_window = {true, pos.x, pos.y, size.x, size.y};
			}
			ImGui::Text("Hello from another window!");
			if (ImGui::Button("Close Me"))
				m_ShowAnotherWindow = false;
			ImGui::End();
		}

		ImGui::Begin("Input Text", &m_StrTest);
		{
			static std::string dynamicString = "Hello World";
			ImGui::InputText("Dynamic String Input", &dynamicString);
			ImGui::Text("You entered: %s", dynamicString.c_str());
		}
		ImGui::End();
		m_Imgui.render(m_Wd, m_Vk, m_ClearColor);
	}
}


int App::destroy() {
	bool const reopen_requested = m_MenuBar.request_reopen;

	// Stop image workers before the rest of teardown so no job is mid-flight.
	img::ImageJobSystem::instance().shutdown();

	vkDeviceWaitIdle(m_Vk.device);
	m_MenuBar.Shutdown();
	m_State.show_demo_window    = m_ShowDemoWindow;
	m_State.show_another_window = m_ShowAnotherWindow;
	m_State.vsync               = m_Vsync;
	m_State.clear_color = WindowStateToml::ColorToml {static_cast<int>(std::lround(m_ClearColor.x * 255.0f + 0.5f)),
		static_cast<int>(std::lround(m_ClearColor.y * 255.0f + 0.5f)),
		static_cast<int>(std::lround(m_ClearColor.z * 255.0f + 0.5f)),
		static_cast<int>(std::lround(m_ClearColor.w * 255.0f + 0.5f))};
	m_StyleEditor.ExportLayout(&m_State);
	m_MenuBar.ExportHistory(&m_State);
	m_MenuBar.ExportRuntimeConfig(&m_State);

	// Session-only override: a --file-browser/--no-file-browser flag forced the
	// visibility for this run only — restore the persisted value so it survives.
	if (m_Opts.file_browser)
		m_State.show_file_explorer_window = m_OriginalShowFileExplorer;

	SaveWindowStateToml(m_StatePath, m_State);
	ImPlot::DestroyContext();
	m_Imgui.shutdown();
	m_Vk.cleanup_window(m_Wd);
	m_Vk.cleanup();
	m_Sdl.shutdown();

	return reopen_requested ? App::k_reopen_exit_code : 0;
}


int App::run() {
	if (!KickStart()) {
		// TODO(you): startup-failure policy — your design decision (see chat).
		// KickStart() can fail AFTER it has already started the image engine and
		// brought up SDL/Vulkan (e.g. surface creation fails). Decide how much to
		// unwind here before returning a non-zero code.
		return 1;
	}

	tick();

	return destroy();
}
