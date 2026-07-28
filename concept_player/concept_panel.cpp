#include "pch.hpp" // NOLINT

#include "concept_panel.hpp"

#include "motion.hpp"

ConceptPanel::ConceptPanel(std::string id, std::string title, PanelEdge edge, float thickness)
    : m_id(std::move(id))
    , m_title(std::move(title))
    , m_edge(edge)
    , m_thickness(thickness)
    , m_reveal(edge, thickness)
{
    m_owner_id = ImHashStr(m_id.c_str());
}

void ConceptPanel::set_placement(PanelPlacement placement)
{
    if (placement == m_placement)
        return;
    if (placement == PanelPlacement::PoppedOut && !allow_popout)
        return;
    m_placement               = placement;
    m_placement_changed_frame = ImGui::GetFrameCount();
    m_open                    = true;
}

void ConceptPanel::toggle_placement()
{
    set_placement(m_placement == PanelPlacement::Overlay ? PanelPlacement::PoppedOut
                                                         : PanelPlacement::Overlay);
}

bool ConceptPanel::begin(const PanelBounds &stage, float dt)
{
    m_style_pushed = false;
    if (!m_enabled)
        return false;

    const std::string window_id = m_title + "###" + m_id;

    if (m_placement == PanelPlacement::PoppedOut) {
        // THIS is what actually guarantees an OS window, and it must be set before
        // Begin(): ImGui decides whether to create a viewport up front, so
        // ImGuiViewportFlags_NoAutoMerge is only settable through a window class.
        //
        // Positioning the window outside the main viewport (below) is enough on X11,
        // but NOT on Wayland: there the compositor owns window placement and
        // SDL_GetWindowPosition cannot report where the window actually went, so ImGui
        // reads the popped-out window back as overlapping the host and auto-merge
        // reclaims it a frame later. Measured on this machine: the platform-window
        // count decayed 4 -> 3 -> 2 -> 1 within a second of launch, i.e. every torn-off
        // panel silently snapped back into the main window. NoAutoMerge pins it open on
        // both backends.
        ImGuiWindowClass window_class;
        window_class.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        ImGui::SetNextWindowClass(&window_class);

        // A hint for where it should land. Honoured on X11; on Wayland the compositor
        // decides and this is ignored — which is fine, because it is no longer what
        // creates the viewport.
        if (ImGui::GetFrameCount() <= m_placement_changed_frame + 1) {
            const ImGuiViewport *vp = ImGui::GetMainViewport();
            const float          x  = (m_edge == PanelEdge::Left)
                                          ? vp->Pos.x - m_thickness - 32.0f
                                          : vp->Pos.x + vp->Size.x + 32.0f;
            ImGui::SetNextWindowPos({x, vp->Pos.y + 64.0f}, ImGuiCond_Always);
            ImGui::SetNextWindowSize({m_thickness, std::max(vp->Size.y * 0.6f, 240.0f)},
                                     ImGuiCond_Always);
            ImGui::SetNextWindowFocus();
        }

        if (!ImGui::Begin(window_id.c_str(), &m_open, ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::End();
            return false;
        }
        // A window the user closed with its [x] belongs back on the stage, not gone.
        if (!m_open) {
            m_open = true;
            set_placement(PanelPlacement::Overlay);
        }
        return true;
    }

    // ── Overlay placement ────────────────────────────────────────────────────
    const float shown = m_reveal.update(m_owner_id, stage, m_pinned, dt);
    if (shown <= 0.002f)
        return false; // fully retracted — draw nothing at all

    const PanelBounds slot = m_reveal.slot(stage, shown);

    // Pin the overlay to the main viewport. Without this, a panel mid-slide sits
    // partly OUTSIDE the host viewport's rect — which is ImGui's cue to give it its own
    // platform window. Measured before this line: the platform-window count spiked to 4
    // on startup and fell back to 1, i.e. three OS windows flickered into existence for
    // a few frames while the panels slid in. An overlay is never its own window; only
    // PoppedOut is.
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    ImGui::SetNextWindowPos(slot.min, ImGuiCond_Always);
    ImGui::SetNextWindowSize(slot.size(), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(background_alpha * shown);

    // Fading the whole panel (not just its background) is what turns the slide into a
    // single motion rather than a moving box whose contents pop in at full strength.
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * shown);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    m_style_pushed = true;

    constexpr ImGuiWindowFlags k_overlay_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar;

    if (!ImGui::Begin(window_id.c_str(), nullptr, k_overlay_flags | extra_window_flags)) {
        ImGui::End();
        ImGui::PopStyleVar(3);
        m_style_pushed = false;
        return false;
    }

    return true;
}

void ConceptPanel::end()
{
    ImGui::End();
    if (m_style_pushed) {
        ImGui::PopStyleVar(3);
        m_style_pushed = false;
    }
}

void ConceptPanel::draw_header()
{
    const float dt = ImGui::GetIO().DeltaTime;

    // Row extents captured before the title is emitted: after an item is drawn the
    // cursor has wrapped to the next line, so GetContentRegionAvail() no longer
    // measures what is left of THIS row and right-aligning off it overflows.
    const float row_left  = ImGui::GetCursorPosX();
    const float row_width = ImGui::GetContentRegionAvail().x;

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.78f, 0.88f, 1.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(m_title.c_str());
    ImGui::PopStyleColor();

    // Right-aligned button cluster. Both buttons animate their tint rather than
    // snapping, so the header matches the motion language of the panel itself.
    const float btn_w     = ImGui::GetFrameHeight();
    const float spacing   = ImGui::GetStyle().ItemSpacing.x;
    const float cluster   = btn_w * 2.0f + spacing;
    const float cluster_x = row_left + row_width - cluster;
    // Title's right edge in window-local coords, to decide whether the cluster still
    // fits on this row without overlapping it.
    const float title_end = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    if (cluster_x > title_end + 8.0f) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(cluster_x);

        const ImVec4 on_col  = {0.35f, 0.62f, 0.95f, 1.0f};
        const ImVec4 off_col = {0.22f, 0.24f, 0.28f, 1.0f};

        ImGui::PushStyleColor(ImGuiCol_Button,
                              Motion::tween_color(m_owner_id,
                                                  ImHashStr("pin_col"),
                                                  m_pinned ? on_col : off_col,
                                                  0.18f,
                                                  EasePreset::OutQuad,
                                                  dt,
                                                  off_col));
        if (ImGui::Button(m_pinned ? "*" : "o", {btn_w, btn_w}))
            m_pinned = !m_pinned;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(m_pinned ? "Unpin — let this panel auto-hide again"
                                       : "Pin — keep this panel out");

        if (!allow_popout) {
            ImGui::Separator();
            return;
        }

        ImGui::SameLine(0.0f, spacing);

        const bool popped = m_placement == PanelPlacement::PoppedOut;
        ImGui::PushStyleColor(ImGuiCol_Button,
                              Motion::tween_color(m_owner_id,
                                                  ImHashStr("pop_col"),
                                                  popped ? on_col : off_col,
                                                  0.18f,
                                                  EasePreset::OutQuad,
                                                  dt,
                                                  off_col));
        if (ImGui::Button(popped ? "[<]" : "[>]", {btn_w, btn_w}))
            toggle_placement();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(popped ? "Dock back onto the video"
                                     : "Pop out into its own window");
    }

    ImGui::Separator();
}
