#pragma once

#include "pch.hpp"

#include "app_coordinator.hpp"
#include "fps_plot.hpp"
#include "imgui_context.hpp"
#include "managed_thread.hpp"
#include "sdl3_context.hpp"
#include "style_editor.hpp"
#include "thread_reflection_panel.hpp"
#include "vulkan_context.hpp"
#include "window_state_toml.hpp"

class AppContext;

class App {
public:
	static constexpr int k_reopen_exit_code = 42;

	App();

	// Full application lifecycle: KickStart() -> tick() loop -> destroy().
	// Returns the process exit code (k_reopen_exit_code asks main() to relaunch).
	int run();

protected:
	// One-time startup: image engine, SDL window, Vulkan/ImGui, UI panels and
	// persisted state. Returns false if any required backend failed to come up.
	bool KickStart();

	// The main loop: pump SDL events and render frames until m_Done is set.
	void tick();

	// Ordered teardown of everything KickStart() built; returns the exit code.
	int destroy();

private:
	AppContext* m_AppContext = nullptr;

	// Platform + rendering backends. Default-constructed here (cheap), then
	// actually initialised inside KickStart(); torn down in destroy().
	sdl3_context              m_Sdl;
	vulkan_context            m_Vk;
	imgui_context             m_Imgui;
	VkSurfaceKHR              m_Surface = VK_NULL_HANDLE;
	ImGui_ImplVulkanH_Window* m_Wd      = nullptr;

	// UI panels / editors drawn every frame.
	FpsPlot               m_FpsPlot;
	ThreadReflectionPanel m_ThreadPanel;
	StyleEditor           m_StyleEditor;
	AppCoordinator        m_MenuBar;

	// Persisted window/session state and the toml path it round-trips through.
	WindowStateToml m_State;
	std::string     m_StatePath;

	// Runtime UI flags.
	ImVec4 m_ClearColor        = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
	bool   m_Done              = false;
	bool   m_ShowDemoWindow    = false;
	bool   m_ShowAnotherWindow = false;
	bool   m_Vsync             = false;
	bool   m_StrTest           = false;
	bool   m_ShowThreadPanel   = false;

	// Shift-drag window-move state.
	bool  m_IsDragging = false;
	float m_DragStartX = 0.0f;
	float m_DragStartY = 0.0f;

	// Session uptime origin, stamped in KickStart().
	std::chrono::steady_clock::time_point m_AppStartTime;

#ifdef _DEBUG
	// Session-lifetime demo thread so the reflection panel is populated on launch.
	std::unique_ptr<ManagedThread> m_DebugDemoThread;
#endif
};
