#pragma once

/// Where a panel currently lives — the concept's "new window" idea.
///
/// A panel is not a fixed piece of the player's chrome. It is a body of content that
/// can either ride over the video as an auto-hiding overlay, or be torn off into its
/// own OS window that stays put while the player runs (useful on a second monitor,
/// and the reason this program uses the SDL_GPU3 ImGui backend — SDL_Renderer3 has
/// no multi-viewport support, so PoppedOut would silently degrade to a floating
/// window trapped inside the main one).
enum class PanelPlacement {
    Overlay,  ///< anchored to its edge, auto-hides, slides in and out
    PoppedOut ///< a real OS window: always visible, movable, never auto-hides
};
