#pragma once

#include "pch.hpp"

class ConfigRuntime;

/// View for the runtime configuration window.
///
/// Holds the UI window configuration state: every frame it binds ImGui widgets directly to the
/// caller-supplied ConfigRuntime model's members (it is a friend of ConfigRuntime),
/// so edits land on the single source of truth that ApplyLayout/ExportLayout persist.
class ConfigRuntimeUiContext {

	public:

		/// Constructor responsible for initializing UI sizing and window constraints.
		ConfigRuntimeUiContext();

		/// Draw the runtime config window (no-op when the model is closed).
		/// `cfg` MUST be the same ConfigRuntime the menu toggles — i.e. the single
		/// registry-owned instance reached via
		/// MemoryManagement::GetInstance<ConfigRuntime>() — so its IsOpen flag and
		/// callbacks are the ones that were wired up.
		void DrawUi(ConfigRuntime* cfg);

	private:

		// --- UI State & Configuration Variables ---
		ImVec2 m_initial_window_size;
		ImVec2 m_default_image_hover_preview_size;

		// --- Private Sub-section Renderers ---
		void RenderImagePreviewSizeSection(ConfigRuntime* cfg);
		void RenderVideoPreviewSizeSection(ConfigRuntime* cfg);
		void RenderHoverPreviewSection(ConfigRuntime* cfg);
		void RenderVideoPlaybackSection(ConfigRuntime* cfg);
		void RenderFileExplorerThumbnailSizeSection(ConfigRuntime* cfg);
		void RenderThumbnailCacheSection(ConfigRuntime* cfg);
		void RenderFileExplorerCacheSection(ConfigRuntime* cfg);
		void RenderVideoCacheSection(ConfigRuntime* cfg);
		void RenderPlaybackPersistenceSection(ConfigRuntime* cfg);
		void RenderPlaybackControlsSection(ConfigRuntime* cfg);
		void RenderHistoryMetadataSection(ConfigRuntime* cfg);
		void RenderApplicationSection(ConfigRuntime* cfg);
};