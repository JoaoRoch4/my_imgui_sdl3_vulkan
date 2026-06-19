#pragma once

#include "pch.hpp"

class ConfigRuntime;

/// View for the runtime configuration window.
///
/// Holds no state of its own: every frame it binds ImGui widgets directly to the
/// caller-supplied ConfigRuntime model's members (it is a friend of ConfigRuntime),
/// so edits land on the single source of truth that ApplyLayout/ExportLayout persist.
class ConfigRuntimeUiContext {

    public:

	/// Draw the runtime config window (no-op when the model is closed).
	/// `cfg` MUST be the same ConfigRuntime the menu toggles — i.e. the single
	/// registry-owned instance reached via
	/// MemoryManagement::GetInstance<ConfigRuntime>() — so its IsOpen flag and
	/// callbacks are the ones that were wired up.
	void DrawUi(ConfigRuntime *cfg);
};
