#include "pch.hpp" // NOLINT

#include "concept_player_app.hpp"

#include "frame_pattern.hpp"
#include "motion.hpp"

ConceptPlayerApp::ConceptPlayerApp()
    : m_menu_panel("menu_bar", "Menu", PanelEdge::Top, 30.0f)
    , m_scene_panel("scene_view", "Scene view", PanelEdge::Left, 236.0f)
    , m_folder_panel("folder_view", "Folder view", PanelEdge::Right, 268.0f)
    , m_transport_panel("transport", "Controls", PanelEdge::Bottom, 104.0f)
{
    // The transport is the one panel that should come back for ANY movement — the
    // viewer stirring means "show me the controls", not "show me the left panel".
    m_transport_panel.reveal().reveal_on_any_motion = true;
    m_transport_panel.reveal().hide_delay           = 2.0f;

    // ── Fullscreen menu bar ──────────────────────────────────────────────────
    // Windowed, the menu bar is ordinary chrome on the player window. Fullscreen there
    // is no chrome to put it on, so it becomes a panel like the others: hidden until
    // the cursor reaches for the top edge, then sliding down over the picture.
    //
    // It is much more transparent than the side panels. Those are content you read;
    // this is a strip of labels you glance at, and it sits directly over the video, so
    // it only has to stay legible — not blot out what is behind it.
    m_menu_panel.extra_window_flags = ImGuiWindowFlags_MenuBar;
    m_menu_panel.allow_popout       = false;
    m_menu_panel.background_alpha   = 0.34f;
    // Shorter reach than a side panel: the top edge is easy to hit, and a deep band
    // would drop the menu over the picture every time the cursor drifted upward.
    m_menu_panel.reveal().edge_margin    = 34.0f;
    m_menu_panel.reveal().hide_delay     = 1.1f;
    m_menu_panel.reveal().slide_duration = 0.26f;

    std::vector<MediaTile> folder;
    static constexpr std::array<const char *, 9> k_names{
        "coastline_dawn.mp4", "harbour_timelapse.mkv", "ridge_pass.mp4",
        "night_market.mov",   "salt_flats.mp4",        "old_town_walk.mkv",
        "storm_front.mp4",    "river_delta.webm",      "last_light.mp4"};
    folder.reserve(k_names.size());
    for (std::size_t i = 0; i < k_names.size(); ++i) {
        MediaTile tile;
        tile.name     = k_names[i];
        tile.seed     = static_cast<std::uint32_t>(i * 7919u + 13u);
        // Spread of lengths so the scene strip is short for some clips and long for
        // others — that difference is exactly what the strip's scrolling has to cope with.
        tile.duration = 45.0 + static_cast<double>((i * 37) % 190);
        folder.push_back(std::move(tile));
    }
    m_shelf.set_items(std::move(folder));
    load_index(0);
}

bool ConceptPlayerApp::pop_out_panel(std::string_view name)
{
    ConceptPanel *panel = nullptr;
    if (name == "scene")
        panel = &m_scene_panel;
    else if (name == "folder")
        panel = &m_folder_panel;
    else if (name == "controls")
        panel = &m_transport_panel;

    if (panel == nullptr)
        return false;
    panel->set_placement(PanelPlacement::PoppedOut);
    return true;
}

void ConceptPlayerApp::pin_all(bool pinned)
{
    m_menu_panel.set_pinned(pinned);
    m_scene_panel.set_pinned(pinned);
    m_folder_panel.set_pinned(pinned);
    m_transport_panel.set_pinned(pinned);
}

void ConceptPlayerApp::load_index(int index)
{
    const auto &items = m_shelf.items();
    if (items.empty())
        return;
    m_current = std::clamp(index, 0, static_cast<int>(items.size()) - 1);
    const MediaTile &tile = items[static_cast<std::size_t>(m_current)];
    m_clock.load(tile.seed, tile.duration, tile.name);
    m_stage.toast(tile.name);
}

void ConceptPlayerApp::switch_relative(int delta)
{
    const int count = static_cast<int>(m_shelf.items().size());
    if (count <= 0)
        return;
    load_index((m_current + delta % count + count) % count);
}

void ConceptPlayerApp::draw()
{
    const float dt = ImGui::GetIO().DeltaTime;
    m_clock.tick(dt);
    m_scene.sync(m_clock);

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    if (!m_fullscreen)
        flags |= ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    const bool player_open = ImGui::Begin("##concept_player", nullptr, flags);
    ImGui::PopStyleVar(2);

    if (!player_open) {
        ImGui::End();
        return;
    }

    // Windowed: the menu bar is chrome on the player window, always there.
    // Fullscreen: it is drawn later, as an auto-hiding panel over the picture.
    if (!m_fullscreen && ImGui::BeginMenuBar()) {
        draw_menu_items();
        ImGui::EndMenuBar();
    }

    // The stage is the whole content area. Panels ride OVER it rather than shrinking
    // it — that is the point of auto-hide, and it is why the picture never reflows
    // when a panel appears.
    const ImVec2      p0 = ImGui::GetCursorScreenPos();
    const ImVec2      av = ImGui::GetContentRegionAvail();
    const PanelBounds stage{p0, {p0.x + av.x, p0.y + av.y}};

    m_stage.draw(stage, m_clock, m_fullscreen, dt);

    // Note: there is no hover-preview window to draw here. The preview runs INLINE, in
    // the folder tile itself (see FolderShelf) — nothing floats over the video.

    if (m_show_hot_zones) {
        // Debug aid: the invisible bands that trigger each panel. Worth keeping while
        // tuning edge_margin — an auto-hide you cannot see the trigger for is very hard
        // to reason about.
        ImDrawList  *fg = ImGui::GetForegroundDrawList();
        const ImU32  col = ImGui::GetColorU32(ImVec4(0.35f, 0.85f, 1.0f, 0.16f));
        for (const ConceptPanel *panel :
             {&m_menu_panel, &m_scene_panel, &m_folder_panel, &m_transport_panel}) {
            if (panel->placement() != PanelPlacement::Overlay || !panel->enabled())
                continue;
            const PanelBounds zone =
                const_cast<ConceptPanel *>(panel)->reveal().hot_zone(stage);
            fg->AddRectFilled(zone.min, zone.max, col);
            fg->AddRect(zone.min, zone.max, ImGui::GetColorU32(ImVec4(0.35f, 0.85f, 1.0f, 0.5f)));
        }
    }

    handle_shortcuts();

    ImGui::End();

    // Panels are top-level windows drawn after the stage, so they sit above it and can
    // be torn off into their own viewports.
    //
    // The transport goes FIRST because the side panels have to know how much of the
    // bottom edge it is currently occupying. In the sketch the controls own the full
    // width of the very bottom and the scene / folder columns stop where the progress
    // bar begins — drawing the side panels over the transport instead buries the
    // transport buttons and the volume slider under them.
    draw_transport_panel(stage, dt);
    draw_menu_panel(stage, dt);

    // The side columns are laid out against a stage shortened by whatever the transport
    // and the menu bar currently occupy, so nothing ever overlaps them mid-slide.
    PanelBounds side_stage = stage;
    side_stage.max.y -= m_transport_panel.occupied();
    side_stage.min.y += m_menu_panel.occupied();
    draw_scene_panel(side_stage, dt);
    draw_folder_panel(side_stage, dt);

    // Reclaim Motion channels for tiles that have scrolled out of existence.
    Motion::gc(600);
}

void ConceptPlayerApp::draw_menu_panel(const PanelBounds &stage, float dt)
{
    // Only fullscreen. Windowed, the bar lives on the player window itself and this
    // panel stays out of the way entirely.
    if (!m_fullscreen)
        return;

    // Zero padding: the panel is a menu bar and nothing else, so any window padding
    // would show as a band of tinted background above and below the labels.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool open = m_menu_panel.begin(stage, dt);
    ImGui::PopStyleVar();

    if (!open)
        return;

    if (ImGui::BeginMenuBar()) {
        draw_menu_items();
        ImGui::EndMenuBar();
    }
    m_menu_panel.end();
}

void ConceptPlayerApp::draw_menu_items()
{
    if (ImGui::BeginMenu("File")) {
        if (ImGui::BeginMenu("Open recent")) {
            const auto &items = m_shelf.items();
            for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                if (ImGui::MenuItem(items[static_cast<std::size_t>(i)].name.c_str(), nullptr,
                                    i == m_current))
                    load_index(i);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q"))
            m_quit_requested = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Fullscreen", "F", m_fullscreen))
            m_fullscreen = !m_fullscreen;
        ImGui::Separator();

        for (ConceptPanel *panel :
             {&m_menu_panel, &m_scene_panel, &m_folder_panel, &m_transport_panel}) {
            ImGui::PushID(panel);
            bool enabled = panel->enabled();
            if (ImGui::MenuItem(panel->title().c_str(), nullptr, enabled))
                panel->set_enabled(!enabled);

            if (ImGui::BeginMenu("  options")) {
                bool pinned = panel->pinned();
                if (ImGui::MenuItem("Pin open", nullptr, pinned))
                    panel->set_pinned(!pinned);
                if (panel->allow_popout) {
                    const bool popped = panel->placement() == PanelPlacement::PoppedOut;
                    if (ImGui::MenuItem("Pop out into its own window", nullptr, popped))
                        panel->toggle_placement();
                }
                ImGui::Separator();
                ImGui::SliderFloat("hide delay", &panel->reveal().hide_delay, 0.2f, 6.0f, "%.1f s");
                ImGui::SliderFloat("edge margin", &panel->reveal().edge_margin, 8.0f, 160.0f,
                                   "%.0f px");
                ImGui::SliderFloat("slide", &panel->reveal().slide_duration, 0.05f, 0.8f, "%.2f s");
                ImGui::EndMenu();
            }
            ImGui::PopID();
        }

        ImGui::Separator();
        ImGui::MenuItem("Show reveal hot zones", nullptr, &m_show_hot_zones);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Motion")) {
        // Surfacing this is the point: the concept is meant to prove out the motion
        // layer, so whether it is running on ImAnim or the fallback must be visible.
        ImGui::TextDisabled("backend");
        ImGui::SameLine();
        if (Motion::using_imanim()) {
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1.0f), "ImAnim (external/ImAnim)");
        } else {
            ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f), "built-in fallback");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("external/ImAnim is not vendored in this checkout.\n"
                                  "Drop it in and re-run CMake: every Motion:: call\n"
                                  "forwards to iam_tween_* with no call-site change.");
        }
        ImGui::Text("live channels: %d", Motion::live_channel_count());
        ImGui::Separator();
        ImGui::SliderFloat("scene step", &m_scene.step_seconds, 1.0f, 30.0f, "+%.0f sec");
        ImGui::SliderFloat("hover dwell", &m_shelf.hover_dwell, 0.0f, 1.2f, "%.2f s");
        ImGui::SliderFloat("preview fade", &m_shelf.preview_fade, 0.05f, 0.8f, "%.2f s");
        ImGui::SliderFloat("seek bar idle", &m_transport.bar_thickness_idle, 1.0f, 16.0f, "%.0f px");
        ImGui::SliderFloat("seek bar hover", &m_transport.bar_thickness_hover, 2.0f, 28.0f,
                           "%.0f px");
        ImGui::EndMenu();
    }

    // Right-aligned status: what is playing, and the panel placements at a glance.
    {
        const std::string status =
            m_clock.title() + "   " + PlaybackClock::format_time(m_clock.position()) + " / " +
            PlaybackClock::format_time(m_clock.duration());
        const float w = ImGui::CalcTextSize(status.c_str()).x;
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > w + 16.0f) {
            ImGui::SameLine(0.0f, avail - w - 8.0f);
            ImGui::TextDisabled("%s", status.c_str());
        }
    }
}

void ConceptPlayerApp::draw_scene_panel(const PanelBounds &stage, float dt)
{
    if (!m_scene_panel.begin(stage, dt))
        return;
    m_scene_panel.draw_header();
    const double clicked = m_scene.draw(m_clock, dt);
    if (clicked >= 0.0) {
        m_clock.seek_to(clicked);
        m_stage.toast(PlaybackClock::format_time(clicked));
    }
    m_scene_panel.end();
}

void ConceptPlayerApp::draw_folder_panel(const PanelBounds &stage, float dt)
{
    // A hidden shelf needs no preview bookkeeping: the preview lives inside a tile, so
    // it stops existing the moment the tiles stop being drawn.
    if (!m_folder_panel.begin(stage, dt))
        return;
    m_folder_panel.draw_header();
    const int clicked = m_shelf.draw(m_current, dt);
    if (clicked >= 0)
        load_index(clicked);
    m_folder_panel.end();
}

void ConceptPlayerApp::draw_transport_panel(const PanelBounds &stage, float dt)
{
    // A scrub in progress pins the bar open, so the controls cannot slide away from
    // under the cursor mid-drag.
    if (m_transport.scrubbing())
        m_transport_panel.reveal().poke();

    if (!m_transport_panel.begin(stage, dt))
        return;
    if (m_transport_panel.placement() == PanelPlacement::PoppedOut)
        m_transport_panel.draw_header();
    const int delta = m_transport.draw(m_clock, m_stage, m_fullscreen, dt);
    if (delta != 0)
        switch_relative(delta);
    m_transport_panel.end();
}

void ConceptPlayerApp::handle_shortcuts()
{
    // Only when no text field or slider has the keyboard, so typing in the View menu's
    // sliders does not scrub the clip.
    if (ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive())
        return;

    if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
        m_clock.toggle_pause();
        m_stage.toast(m_clock.paused() ? "Paused" : "Playing");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        m_clock.seek_by(-m_transport.seek_step_seconds);
        m_stage.toast("Seek -" + std::to_string(m_transport.seek_step_seconds) + "s");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        m_clock.seek_by(m_transport.seek_step_seconds);
        m_stage.toast("Seek +" + std::to_string(m_transport.seek_step_seconds) + "s");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F)) {
        m_fullscreen = !m_fullscreen;
        m_stage.toast(m_fullscreen ? "Fullscreen" : "Windowed");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && m_fullscreen) {
        m_fullscreen = false;
        m_stage.toast("Windowed");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        // One key to hold every panel out — the fastest way to compare the sketched
        // layout against the auto-hiding one.
        const bool pin = !m_scene_panel.pinned();
        pin_all(pin);
        m_stage.toast(pin ? "Panels pinned" : "Panels auto-hide");
    }
    // 1 / 2 / 3 flip a panel between riding over the video and being its own OS
    // window. Bound to keys because that comparison — docked overlay versus torn-off
    // window — is the single judgement this concept exists to support, and it should
    // not cost a trip through a menu each time.
    if (ImGui::IsKeyPressed(ImGuiKey_1)) {
        m_scene_panel.toggle_placement();
        m_stage.toast(m_scene_panel.placement() == PanelPlacement::PoppedOut
                          ? "Scene view popped out"
                          : "Scene view docked");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_2)) {
        m_folder_panel.toggle_placement();
        m_stage.toast(m_folder_panel.placement() == PanelPlacement::PoppedOut
                          ? "Folder view popped out"
                          : "Folder view docked");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_3)) {
        m_transport_panel.toggle_placement();
        m_stage.toast(m_transport_panel.placement() == PanelPlacement::PoppedOut
                          ? "Controls popped out"
                          : "Controls docked");
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Q) && ImGui::GetIO().KeyCtrl)
        m_quit_requested = true;
}
