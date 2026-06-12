#pragma once

#include "pch.hpp"

#include "app_coordinator.hpp"
#include "app_runtime_state.hpp"
#include "fps_plot.hpp"
#include "imgui_context.hpp"
#include "managed_thread.hpp"
#include "sdl3_context.hpp"
#include "startup_options.hpp"
#include "style_editor.hpp"
#include "thread_reflection_panel.hpp"
#include "vulkan_context.hpp"
#include "window_state_toml.hpp"
#include "Memory_management.hpp"


class AppContext;

class App {
public:
	static constexpr int k_reopen_exit_code = 42;

	// Resolved command-line overrides. Passed in from start() so the same flags
	// apply to every App instance across a reopen.
	explicit App(StartupOptions opts = {});

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

	bool Alloc();

private:
	// Injected config / singletons / the registry handle itself — NOT
	// registry-owned, so they keep their existing lifetimes.
	AppContext* m_AppContext = nullptr;
	// Resolved command-line overrides, applied in KickStart() after the TOML
	// load. See StartupOptions.
	StartupOptions    m_Opts;
	MemoryManagement* m_mem = nullptr;

	// Everything below is allocated in Alloc() via m_mem->PushGet<T>() at the
	// start of each run(), cached here as a raw observer pointer, and dropped in
	// destroy() via m_mem->Release<T>(). The registry owns the storage; App only
	// holds the addresses. Re-created fresh on every reopen iteration.

	// Platform + rendering backends.
	sdl3_context*   m_Sdl   = nullptr;
	vulkan_context* m_Vk    = nullptr;
	imgui_context*  m_Imgui = nullptr;

	// UI panels / editors drawn every frame.
	FpsPlot*               m_FpsPlot     = nullptr;
	ThreadReflectionPanel* m_ThreadPanel = nullptr;
	StyleEditor*           m_StyleEditor = nullptr;
	AppCoordinator*        m_MenuBar     = nullptr;

	// Persisted window/session state (round-trips through TOML).
	WindowStateToml* m_State = nullptr;

	// Loose runtime flags + render handles, bundled into one registry entry.
	// See AppRuntimeState. Replaces the former scattered m_Surface/m_Wd/
	// m_StatePath/m_ClearColor/m_Done/... members.
	AppRuntimeState* m_Rt = nullptr;

#ifdef _DEBUG
	// Session-lifetime demo thread so the reflection panel is populated on launch.
	// A plain unique_ptr member (not registry-owned); re-spawned each KickStart().
	std::unique_ptr<ManagedThread> m_DebugDemoThread;
#endif
};
