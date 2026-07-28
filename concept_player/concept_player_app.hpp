#pragma once

#include "concept_panel.hpp"
#include "folder_shelf.hpp"
#include "panel_bounds.hpp"
#include "pch.hpp"
#include "playback_clock.hpp"
#include "scene_strip.hpp"
#include "transport_bar.hpp"
#include "video_stage.hpp"

/// Composes the sketched player: menu bar, video stage, three auto-hiding panels, and
/// the floating hover preview.
///
/// The app owns the panels and the widgets separately on purpose. A ConceptPanel knows
/// about placement and reveal and nothing about video; SceneStrip and FolderShelf know
/// about content and nothing about where they are drawn. That seam is what lets a panel
/// be popped into its own OS window without the widget inside it noticing.
class ConceptPlayerApp {
public:
    ConceptPlayerApp();

    /// Draws one frame. Call between ImGui::NewFrame() and ImGui::Render().
    void draw();

    /// The window should be fullscreen — the caller applies it to the OS window.
    [[nodiscard]] bool fullscreen() const { return m_fullscreen; }

    /// The user asked to quit from the File menu.
    [[nodiscard]] bool quit_requested() const { return m_quit_requested; }

    /// Starts a panel already torn off into its own OS window. Accepts "scene",
    /// "folder" or "controls"; returns false for anything else.
    ///
    /// Exists because the arrangement worth evaluating is often the multi-monitor one,
    /// and reaching it by hand every launch is friction. It is also the only way to
    /// exercise pop-out under Wayland from a script: the compositor owns window
    /// placement there and offers no input-injection path.
    bool pop_out_panel(std::string_view name);

    /// Pins (or unpins) every panel, as the Tab key does.
    void pin_all(bool pinned);

private:
    /// The File / View / Motion menus plus the right-aligned status. Called from inside
    /// a menu bar — either the docked one on the player window, or the fullscreen menu
    /// panel — so the two never drift apart.
    void draw_menu_items();
    void draw_menu_panel(const PanelBounds &stage, float dt);
    void draw_scene_panel(const PanelBounds &stage, float dt);
    void draw_folder_panel(const PanelBounds &stage, float dt);
    void draw_transport_panel(const PanelBounds &stage, float dt);
    void handle_shortcuts();
    void load_index(int index);
    void switch_relative(int delta);

    PlaybackClock m_clock;
    VideoStage    m_stage;
    SceneStrip    m_scene;
    FolderShelf   m_shelf;
    TransportBar  m_transport;

    ConceptPanel m_menu_panel;
    ConceptPanel m_scene_panel;
    ConceptPanel m_folder_panel;
    ConceptPanel m_transport_panel;

    bool m_fullscreen     = false;
    bool m_quit_requested = false;
    bool m_show_hot_zones = false;
    int  m_current        = 0;
};
