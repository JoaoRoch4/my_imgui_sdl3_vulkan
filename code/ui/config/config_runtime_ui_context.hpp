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
	/// `cfg` MUST be the same ConfigRuntime the menu toggles (AppCoordinator's
	/// m_ctx->Config()) — NOT AppContext::GetInstance(), which is a different
	/// instance whose IsOpen/callbacks are never wired up.
	void DrawUi(ConfigRuntime *cfg);
};
