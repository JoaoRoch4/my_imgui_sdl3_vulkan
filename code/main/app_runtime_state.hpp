#pragma once

#include "pch.hpp"


/**
 * @brief Loose per-session runtime state for App, bundled into one type.
 *
 * The registry (MemoryManagement) keys entries by std::type_index, so it can
 * hold at most one object per C++ type — individual bool/float flags could not
 * each be their own entry. Gathering them here gives them a single distinct
 * type, so the whole bundle becomes one registry-owned object that any class
 * can reach via MemoryManagement::GetInstance<AppRuntimeState>().
 *
 * Everything here is plain value/handle state with a trivial destructor:
 * render handles that point into the (separately owned) vulkan_context, UI
 * toggles round-tripped through the persisted TOML, transient shift-drag
 * bookkeeping and the session uptime origin. A fresh instance is created at the
 * start of every App::run() and released in App::destroy(), so each reopen
 * iteration begins from clean defaults.
 */
struct AppRuntimeState {

	// ---- Render handles (owned elsewhere; observed here) --------------------
	// Vulkan surface for the main window and the ImGui per-window data, which
	// points into vulkan_context::main_window_data. Both are torn down through
	// vulkan_context in App::destroy() before this bundle is released.
	VkSurfaceKHR              surface = VK_NULL_HANDLE;
	ImGui_ImplVulkanH_Window* wd      = nullptr;

	// ---- Persisted-state plumbing -------------------------------------------
	// Absolute path the WindowStateToml round-trips through, and the snapshot of
	// the file-explorer visibility taken BEFORE CLI overrides, so a
	// --file-browser/--no-file-browser flag never rewrites the saved preference.
	std::string statePath;
	bool        originalShowFileExplorer = false;

	// ---- Runtime UI flags (mirrored to/from the TOML) -----------------------
	ImVec4 clearColor        = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
	bool   done              = false;
	bool   showDemoWindow    = false;
	bool   showAnotherWindow = false;
	bool   vsync             = false;
	bool   strTest           = false;
	bool   showThreadPanel   = false;

	// ---- Shift-drag window-move bookkeeping ---------------------------------
	bool  isDragging = false;
	float dragStartX = 0.0f;
	float dragStartY = 0.0f;

	// ---- Session uptime origin, stamped in KickStart() ----------------------
	std::chrono::steady_clock::time_point appStartTime;
};
