#include "pch.hpp"

#include "app.hpp"


#include "Memory_management.hpp"
#include "app_context.hpp"
#include "app_runtime_state.hpp"
#include "image_job_system.hpp"
#include "window_fullscreen_utils.hpp"
#include <bit>
#include <utility>




App::App(StartupOptions opts)
	: m_Opts(std::move(opts)) {
	m_AppContext = AppContext::GetInstance();
	// Allocation is no longer done here: every owned subobject is created in
	// Alloc(), which run() calls at the START of each iteration so a reopen gets
	// a fresh set (see main.cpp's reopen loop). The ctor only wires the injected
	// singleton.
}

bool App::Alloc() {
	// The single, central allocation point for the whole App. Every owned
	// subobject is created through the registry and cached as a raw observer
	// pointer. This call order DEFINES the registration order; destroy() releases
	// in the exact reverse of it.
	m_mem = MemoryManagement::GetPtr();

	m_Rt          = m_mem->PushGet<AppRuntimeState>("AppRuntime");
	m_State       = m_mem->PushGet<WindowStateToml>("WindowState");
	m_Sdl         = m_mem->PushGet<sdl3_context>("Sdl");
	m_Vk          = m_mem->PushGet<vulkan_context>("Vulkan");
	m_Imgui       = m_mem->PushGet<imgui_context>("ImGui");
	m_FpsPlot     = m_mem->PushGet<FpsPlot>("FpsPlot");
	m_ThreadPanel = m_mem->PushGet<ThreadReflectionPanel>("ThreadPanel");
	m_StyleEditor = m_mem->PushGet<StyleEditor>("StyleEditor");
	m_MenuBar     = m_mem->PushGet<AppCoordinator>("MenuBar");

	return m_Rt && m_State && m_Sdl && m_Vk && m_Imgui && m_FpsPlot && m_ThreadPanel && m_StyleEditor && m_MenuBar;
}


bool App::KickStart() {
	// Name the main OS thread so it shows as "MainThread" instead of the process
	// name ("example_sdl3_vu") in the thread reflection panel and debuggers.
	pthread_setname_np(pthread_self(), "MainThread");

	m_Rt->appStartTime = std::chrono::steady_clock::now();

	// Start the parallel image engine (decode/resize/encode worker pool wired to
	// ThreadOverwatch) for the whole session; shut it down in destroy().
	img::ImageJobSystem::instance().start();


	if (!m_Sdl->init("Dear ImGui SDL3+Vulkan example", 1280, 800))
		return false;


	{
		std::vector<char const*> extensions;
		uint32_t                 count = 0;
		char const* const*       exts  = SDL_Vulkan_GetInstanceExtensions(&count);
		extensions.reserve(count);
		for (uint32_t i = 0; i < count; i++)
			extensions.push_back(exts[i]);
		m_Vk->setup(extensions);
	}

	if (SDL_Vulkan_CreateSurface(m_Sdl->window, m_Vk->instance, m_Vk->allocator, &m_Rt->surface) == 0) {
		std::printf("Failed to create Vulkan surface.\n");
		return false;
	}

	int w;
	int h;
	SDL_GetWindowSize(m_Sdl->window, &w, &h);
	m_Rt->wd = &m_Vk->main_window_data;
	m_Vk->setup_window(m_Rt->wd, m_Rt->surface, w, h);
	SDL_SetWindowPosition(m_Sdl->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	SDL_ShowWindow(m_Sdl->window);

	m_Imgui->init(m_Sdl->window, *m_Vk, m_Rt->wd, m_Sdl->main_scale);
	ImPlot::CreateContext();

#ifdef _DEBUG
	// Debug builds: spawn a session-lifetime demo thread so the reflection panel
	// has sample content when it is shown. The panel itself is no longer
	// auto-opened — it appears only when --monitor-thread is passed (see below).
	// The thread stops + joins when this unique_ptr is destroyed or reassigned.
	m_DebugDemoThread = spawn_demo_thread(std::chrono::seconds {0});
#endif

	m_StyleEditor->InitDefaults();
	// Google Noto bases first (the mains); add only the families that resolved at
	// runtime, then Dear ImGui's bundled faces as alternatives.
	std::vector<std::pair<std::string, ImFont*>> fonts;
	if (m_Imgui->font_noto_sans)
		fonts.emplace_back("Noto Sans", m_Imgui->font_noto_sans);
	if (m_Imgui->font_noto_mono)
		fonts.emplace_back("Noto Sans Mono", m_Imgui->font_noto_mono);
	if (m_Imgui->font_noto_serif)
		fonts.emplace_back("Noto Serif", m_Imgui->font_noto_serif);
	fonts.emplace_back("Cousine", m_Imgui->font_cousine);
	fonts.emplace_back("DroidSans", m_Imgui->font_droid_sans);
	fonts.emplace_back("Karla", m_Imgui->font_karla);
	fonts.emplace_back("ProggyClean", m_Imgui->font_proggy_clean);
	fonts.emplace_back("ProggyTiny", m_Imgui->font_proggy_tiny);
	fonts.emplace_back("Roboto", m_Imgui->font_roboto);
	m_StyleEditor->SetFonts(std::move(fonts));

	// Shared cache folder at the project root — deliberately OUTSIDE build/debug,
	// build/release and build/release-log so all three builds read & write the
	// SAME window_state.toml (plus thumbnails and video cache). SDL_GetBasePath()
	// is the executable dir (e.g. <root>/build/debug/); two parents up is <root>.
	auto const exe_dir      = std::filesystem::path(SDL_GetBasePath());
	auto const project_root = exe_dir.parent_path().parent_path();
	auto const cache_dir    = project_root / "cache";
	std::filesystem::create_directories(cache_dir);

	m_Rt->statePath = (cache_dir / "window_state.toml").string();
	LoadWindowStateToml(m_Rt->statePath, *m_State);

	// ---- Apply command-line overrides on top of the loaded TOML -------------
	// Snapshot the persisted file-explorer visibility BEFORE overriding it, so
	// destroy() can restore it and keep the CLI override session-only.
	m_Rt->originalShowFileExplorer = m_State->show_file_explorer_window;
	if (m_Opts.file_browser)
		m_State->show_file_explorer_window = *m_Opts.file_browser;

	// --monitor-thread: force the Threads reflection panel open (release too).
	if (m_Opts.monitor_thread)
		m_Rt->showThreadPanel = true;

	m_StyleEditor->ApplyLayout(*m_State);

	m_Rt->showDemoWindow    = m_State->show_demo_window;
	m_Rt->showAnotherWindow = m_State->show_another_window;
	m_Rt->vsync             = m_State->vsync;
	m_Vk->set_vsync(m_Rt->wd, m_Rt->vsync);

	// Phase-2: the coordinator now pulls StyleEditor / window / vulkan_context /
	// show-flags from the registry itself. App only injects the vsync behaviour.
	m_MenuBar->Setup([this](bool enabled) {
		m_Rt->vsync = enabled;
		m_Vk->set_vsync(m_Rt->wd, m_Rt->vsync);
	});
	m_MenuBar->LoadOpenedFilesHistoryFromToml(m_Rt->statePath);
	m_MenuBar->SetStatePath(m_Rt->statePath);
	m_MenuBar->ApplyHistory(*m_State);
	m_MenuBar->ApplyRuntimeConfig(*m_State);

	// --no-video / --no-media: gate the media-routing choke point. Video is
	// disabled by either flag; images only by --no-media. Subsystems still boot;
	// they just receive nothing to load.
	m_MenuBar->SetMediaPolicy(/*allow_video=*/!(m_Opts.disable_video || m_Opts.disable_media),
		/*allow_image=*/!m_Opts.disable_media);

	// Echo what the flags did to BOTH stdout and the integrated console, then
	// auto-open the console so the feedback is visible. With no flags the list is
	// empty: nothing prints, the console stays as the TOML left it. This runs
	// AFTER ApplyRuntimeConfig (which sets console visibility from TOML) so the
	// auto-open is not clobbered.
	if (auto const args_feedback = describe_startup_options(m_Opts); !args_feedback.empty()) {
		for (auto const& line : args_feedback) {
			std::println("[Args] {}", line);
			m_MenuBar->ConsoleLog(std::format("[Args] {}", line));
		}
		m_MenuBar->ShowConsole();
	}

	m_MenuBar->SetThumbDir(cache_dir / "thumbs");
	m_MenuBar->SetDownloadCacheDir(cache_dir / "video_cache");

	m_Rt->clearColor = m_State->clear_color
		? ImVec4(static_cast<float>(m_State->clear_color->r) / 255.0f,
			  static_cast<float>(m_State->clear_color->g) / 255.0f,
			  static_cast<float>(m_State->clear_color->b) / 255.0f,
			  static_cast<float>(m_State->clear_color->a) / 255.0f)
		: ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

	return true;
}


void App::tick() {
	while (!m_Rt->done) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			m_MenuBar->HandleSdlEvent(event);
			ImGui_ImplSDL3_ProcessEvent(&event);
			ImGuiIO& io = ImGui::GetIO();
			(void)io;

			if (event.type == SDL_EVENT_QUIT)
				m_Rt->done = true;
			if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(m_Sdl->window))
				m_Rt->done = true;

			if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
				if (event.button.button == SDL_BUTTON_LEFT && (SDL_GetModState() & SDL_KMOD_SHIFT)) {
					m_Rt->isDragging = true;
					SDL_GetGlobalMouseState(&m_Rt->dragStartX, &m_Rt->dragStartY);

					// Force ImGui to release active widgets so window dragging can take over.
					ImGui::ClearActiveID();
				}
			}

			if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
				if (event.button.button == SDL_BUTTON_LEFT)
					m_Rt->isDragging = false;
			}

			if (event.type == SDL_EVENT_MOUSE_MOTION && m_Rt->isDragging) {
				float current_x;
				float current_y;
				SDL_GetGlobalMouseState(&current_x, &current_y);

				float delta_x = current_x - m_Rt->dragStartX;
				float delta_y = current_y - m_Rt->dragStartY;

				int win_x;
				int win_y;
				SDL_GetWindowPosition(m_Sdl->window, &win_x, &win_y);

				SDL_SetWindowPosition(m_Sdl->window, win_x + static_cast<int>(delta_x),
					win_y + static_cast<int>(delta_y));

				m_Rt->dragStartX = current_x;
				m_Rt->dragStartY = current_y;
			}

			if (event.type == SDL_EVENT_KEY_DOWN) {
				if (event.key.key == SDLK_F11) {
					toggle_window_fullscreen(m_Sdl->window);
				}
			}
		}

		m_Rt->done |= m_MenuBar->request_quit;

		if (SDL_GetWindowFlags(m_Sdl->window) & SDL_WINDOW_MINIMIZED) {
			SDL_Delay(10);
			continue;
		}

		int fb_width;
		int fb_height;
		SDL_GetWindowSize(m_Sdl->window, &fb_width, &fb_height);
		if (fb_width > 0 && fb_height > 0
			&& (m_Vk->swap_chain_rebuild || m_Rt->wd->Width != fb_width || m_Rt->wd->Height != fb_height))
			m_Vk->resize_window(m_Rt->wd, fb_width, fb_height);

		m_Imgui->new_frame();

		auto const   uptime_now           = std::chrono::steady_clock::now();
		double const uptime_seconds       = std::chrono::duration<double>(uptime_now - m_Rt->appStartTime).count();
		auto const   uptime_total_seconds = static_cast<int>(uptime_seconds);
		int const    uptime_hours         = uptime_total_seconds / 3600;
		int const    uptime_minutes       = (uptime_total_seconds % 3600) / 60;
		int const    uptime_secs          = uptime_total_seconds % 60;

		m_FpsPlot->add_sample(ImGui::GetIO().Framerate);

		m_MenuBar->Build();
		m_FpsPlot->draw(uptime_seconds);
		m_ThreadPanel->draw(&m_Rt->showThreadPanel);

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

		m_StyleEditor->Draw();

		if (m_Rt->showDemoWindow)
			ImGui::ShowDemoWindow(&m_Rt->showDemoWindow);

		{
			static float f       = 0.0f;
			static int   counter = 0;

			if (m_State->hello_world_window.valid) {
				ImGui::SetNextWindowPos({m_State->hello_world_window.x, m_State->hello_world_window.y}, ImGuiCond_Once);
				ImGui::SetNextWindowSize({m_State->hello_world_window.w, m_State->hello_world_window.h}, ImGuiCond_Once);
			}
			ImGui::Begin("Hello, world!");
			{
				ImVec2 pos                  = ImGui::GetWindowPos();
				ImVec2 size                 = ImGui::GetWindowSize();
				m_State->hello_world_window = {true, pos.x, pos.y, size.x, size.y};
			}
			ImGui::Text("This is some useful text.");
			ImGui::Checkbox("Demo Window", &m_Rt->showDemoWindow);
			ImGui::Checkbox("Another Window", &m_Rt->showAnotherWindow);
			ImGui::Checkbox("Style Editor", &m_StyleEditor->IsOpen);
			ImGui::Checkbox("String Test", &m_Rt->strTest);

			if (ImGui::Checkbox("VSync", &m_Rt->vsync))
				m_Vk->set_vsync(m_Rt->wd, m_Rt->vsync);
			ImGui::SliderFloat("float", &f, 0.0f, 1.0f);
			ImGui::ColorEdit3("clear color", &m_Rt->clearColor.x);
			if (ImGui::Button("Button"))
				counter++;
			ImGui::SameLine();
			ImGui::Text("counter = %d", counter);
			ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate,
				ImGui::GetIO().Framerate);
			ImGui::Text("Uptime: %02d:%02d:%02d", uptime_hours, uptime_minutes, uptime_secs);
			ImGui::End();
		}

		if (m_Rt->showAnotherWindow) {
			if (m_State->another_window.valid) {
				ImGui::SetNextWindowPos({m_State->another_window.x, m_State->another_window.y}, ImGuiCond_Once);
				ImGui::SetNextWindowSize({m_State->another_window.w, m_State->another_window.h}, ImGuiCond_Once);
			}
			ImGui::Begin("Another Window", &m_Rt->showAnotherWindow);
			{
				ImVec2 pos              = ImGui::GetWindowPos();
				ImVec2 size             = ImGui::GetWindowSize();
				m_State->another_window = {true, pos.x, pos.y, size.x, size.y};
			}
			ImGui::Text("Hello from another window!");
			if (ImGui::Button("Close Me"))
				m_Rt->showAnotherWindow = false;
			ImGui::End();
		}

		ImGui::Begin("Input Text", &m_Rt->strTest);
		{
			static std::string dynamicString = "Hello World";
			ImGui::InputText("Dynamic String Input", &dynamicString);
			ImGui::Text("You entered: %s", dynamicString.c_str());
		}
		ImGui::End();
		m_Imgui->render(m_Rt->wd, *m_Vk, m_Rt->clearColor);
	}
}


int App::destroy() {
	bool const reopen_requested = m_MenuBar->request_reopen;

	// Stop image workers before the rest of teardown so no job is mid-flight.
	img::ImageJobSystem::instance().shutdown();

	vkDeviceWaitIdle(m_Vk->device);
	m_MenuBar->Shutdown();
	m_State->show_demo_window    = m_Rt->showDemoWindow;
	m_State->show_another_window = m_Rt->showAnotherWindow;
	m_State->vsync               = m_Rt->vsync;
	m_State->clear_color = WindowStateToml::ColorToml {static_cast<int>(std::lround(m_Rt->clearColor.x * 255.0f + 0.5f)),
		static_cast<int>(std::lround(m_Rt->clearColor.y * 255.0f + 0.5f)),
		static_cast<int>(std::lround(m_Rt->clearColor.z * 255.0f + 0.5f)),
		static_cast<int>(std::lround(m_Rt->clearColor.w * 255.0f + 0.5f))};
	m_StyleEditor->ExportLayout(m_State);
	m_MenuBar->ExportHistory(m_State);
	m_MenuBar->ExportRuntimeConfig(m_State);

	// Session-only override: a --file-browser/--no-file-browser flag forced the
	// visibility for this run only — restore the persisted value so it survives.
	if (m_Opts.file_browser)
		m_State->show_file_explorer_window = m_Rt->originalShowFileExplorer;

	SaveWindowStateToml(m_Rt->statePath, *m_State);
	ImPlot::DestroyContext();
	m_Imgui->shutdown();
	m_Vk->cleanup_window(m_Rt->wd);
	m_Vk->cleanup();
	m_Sdl->shutdown();

	// Deterministic registry teardown, the exact reverse of Alloc()'s
	// registration order, so each object's destructor runs HERE (after its
	// explicit cleanup above) rather than at static exit, and the next reopen
	// iteration's Alloc() finds an empty registry — no duplicate type entries.
	m_mem->Release<AppCoordinator>();
	m_mem->Release<StyleEditor>();
	m_mem->Release<ThreadReflectionPanel>();
	m_mem->Release<FpsPlot>();
	m_mem->Release<imgui_context>();
	m_mem->Release<vulkan_context>();
	m_mem->Release<sdl3_context>();
	m_mem->Release<WindowStateToml>();
	m_mem->Release<AppRuntimeState>();

	// Every cached pointer now dangles — null them so any stale use is an obvious
	// nullptr crash, and so the next Alloc() reassigns from scratch.
	m_MenuBar     = nullptr;
	m_StyleEditor = nullptr;
	m_ThreadPanel = nullptr;
	m_FpsPlot     = nullptr;
	m_Imgui       = nullptr;
	m_Vk          = nullptr;
	m_Sdl         = nullptr;
	m_State       = nullptr;
	m_Rt          = nullptr;

	return reopen_requested ? App::k_reopen_exit_code : 0;
}


int App::run() {
	// Central allocation up front, fresh on every iteration of main.cpp's reopen
	// loop. Releasing in destroy() keeps these PushGet calls creating new objects
	// instead of appending duplicates the type-keyed lookup would shadow.
	if (!Alloc())
		return 1;

	if (!KickStart()) {
		// Startup failed after Alloc() (and possibly after the image engine / SDL /
		// Vulkan partially came up). We deliberately do NOT run the full destroy()
		// teardown — Vulkan may be only half-initialised. The process exits with a
		// non-zero code, so the registry is reclaimed at static exit.
		return 1;
	}

	tick();

	return destroy();
}
