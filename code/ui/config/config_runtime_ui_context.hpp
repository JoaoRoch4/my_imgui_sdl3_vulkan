#pragma once

#include "pch.hpp"

/// View for the runtime configuration window.
///
/// Holds no state of its own: every frame it resolves the ConfigRuntime model
/// from AppContext and binds ImGui widgets directly to the model's members
/// (it is a friend of ConfigRuntime), so edits land on the single source of
/// truth that ApplyLayout/ExportLayout persist.
class ConfigRuntimeUiContext {

    public:

	/// Draw the runtime config window (no-op when the model is closed).
	void DrawUi();
};
