/**
 * @file Image_viewer_panel.cpp
 * @brief Implementation of ImageViewerPanel.
 *
 * Each open image is shown in its own resizable ImGui window.
 * The user can:
 *   - Scroll the mouse wheel        → zoom toward / away from the cursor.
 *   - Left-drag (zoom == 1)         → move the ImGui window (image is "draggable").
 *   - Left-drag (zoom > 1)          → pan the image within the canvas.
 *   - Shift + Left-drag (any zoom)  → always move the ImGui window.
 *   - Double-click                  → reset zoom and pan to defaults.
 *
 * The cursor changes to communicate the active drag mode:
 *   ImGuiMouseCursor_ResizeAll  → window-move mode.
 *   ImGuiMouseCursor_Hand       → image-pan mode.
 *
 * -----------------------------------------------------------------------
 * Coordinate spaces
 * -----------------------------------------------------------------------
 *
 *   Screen-space : absolute pixel position on the OS display.
 *   Canvas-space : pixel offset from the top-left corner of the
 *                  InvisibleButton that covers the image drawing area.
 *   Image-space  : pixel coordinate inside the source image
 *                  (range 0 .. width  /  0 .. height).
 *
 * -----------------------------------------------------------------------
 * Core formula
 * -----------------------------------------------------------------------
 *
 *   ppm  =  base_scale * zoom        (pixels-per-image-pixel on screen)
 *
 *   screen_pos  =  canvas_origin + (image_px − offset) * ppm
 *   image_px    =  offset + canvas_px / ppm          ... (*)
 *
 * Pan  — dragging by (dx, dy) screen pixels:
 *   Δoffset = −{dx, dy} / ppm
 *   (minus because dragging RIGHT reveals the LEFT side of the image)
 *
 * Zoom-to-cursor — changing ppm from old→new while keeping image_px
 * at the cursor position C (canvas-space) fixed:
 *   From (*):   offset_new + C/ppm_new = offset_old + C/ppm_old
 *   → Δoffset = C * (1/ppm_old − 1/ppm_new)
 */

#include "Image_viewer_panel.hpp"

#include "imgui_internal.h"
#include "window_state_toml.hpp"

// ============================================================================
// Construction
// ============================================================================

/**
 * @brief Default constructor — starts with an empty image list.
 */
ImageViewerPanel::ImageViewerPanel()
    : m_images{}               // no images open at startup
    , m_next_id{0}             // IDs start at zero and increment monotonically
    , m_requested_focus_id{-1} // -1 means no focus request pending
    , m_context_menu{}
{
}

void ImageViewerPanel::setup(SDL_Window *window)
{
    m_context_menu.setup(window);
}

void ImageViewerPanel::set_on_save_success(
    std::function<void(const std::string &, const std::filesystem::path &)> cb)
{
    m_context_menu.set_on_save_success(std::move(cb));
}

// ============================================================================
// Public lifecycle
// ============================================================================

/**
 * @brief Load a texture from disk and register it as a new viewer window.
 *
 * Builds and verifies a fully initialised ImageEntry before committing it to
 * m_images, so a failed decode never leaves a half-initialised entry in the list.
 *
 * @param path  Path to the image file on disk.
 * @param vk    Active Vulkan context for GPU upload.
 * @return      true on success; false if decoding or GPU upload fails.
 */
bool ImageViewerPanel::add_from_path(const std::filesystem::path &path,
                                     vulkan_context &vk) {
    ImageEntry entry;

    // Show only the filename component as the window title.
    // e.g. "/home/user/photos/cat.png" → "cat.png"
    entry.title = path.filename().string();
    entry.source = path.string();
    entry.kind = "file";
    entry.id = m_next_id++; // stable ID that will never be reused this session
    entry.open = true;      // visible immediately in the same frame

    // Decode pixels from disk and upload an RGBA VkImage to the GPU.
    if (!entry.texture.load(path, vk))
        return false; // decoding failed — discard and report failure

    m_images.push_back(std::move(entry)); // transfer ownership into the managed list
    return true;
}

/**
 * @brief Load a texture from a temp file with a caller-supplied display title.
 *
 * Designed for the URL download flow: the caller downloads a URL to a
 * temp path, hands the path here, then deletes the temp file afterward.
 *
 * @param tmp_path       Path to the temporary image file.
 * @param display_title  Title shown in the ImGui window title bar.
 * @param source_url     Original URL stored for history synchronisation.
 * @param vk             Active Vulkan context for GPU upload.
 * @return               true on success.
 */
bool ImageViewerPanel::add_from_url_temp(const std::filesystem::path &tmp_path,
                                         const std::string &display_title,
                                         const std::string &source_url,
                                         vulkan_context &vk) {
    ImageEntry entry;
    entry.title = display_title; // caller derives this from the URL (e.g. filename part)
    entry.source = source_url;   // store the original URL for history sync
    entry.kind = "url";
    entry.id = m_next_id++;
    entry.open = true;

    // Load from the temp path — the extension must be preserved so the decoder
    // (stb_image / libwebp) can detect the format correctly.
    if (!entry.texture.load(tmp_path, vk))
        return false;

    m_images.push_back(std::move(entry));
    return true;
}

/**
 * @brief Destroy all GPU resources owned by this panel.
 *
 * Must be called before ImGui_ImplVulkan_Shutdown() and before the VkDevice
 * is destroyed.  Safe to call on an already-empty panel.
 *
 * @param vk  Active Vulkan context for resource destruction.
 */
void ImageViewerPanel::shutdown(vulkan_context &vk) {
    // Unload every entry regardless of its open/closed state.
    for (auto &entry : m_images)
        if (entry.texture.is_loaded())
            entry.texture.unload(vk);

    m_images.clear(); // subsequent calls are harmless no-ops
}

/**
 * @brief Free GPU resources for every entry that has been closed by the user.
 *
 * An entry's open flag is set to false when the user clicks the [X] button
 * in the ImGui title bar.  This function waits for the GPU to finish (so the
 * texture is no longer referenced by any in-flight command buffer) before
 * calling unload().  It must be called at the START of each frame, after
 * the frame fence signals.
 *
 * @param vk  Active Vulkan context needed for vkDeviceWaitIdle and unload().
 */
void ImageViewerPanel::evict_closed(vulkan_context &vk) {
    // Fast path: nothing to do if every entry is still open.
    const auto it = std::find_if(m_images.begin(), m_images.end(),
                                 [](const ImageEntry &e) { return !e.open; });
    if (it == m_images.end())
        return;

    // Wait for the GPU to finish all pending work before freeing any texture.
    vkDeviceWaitIdle(vk.device);

    // Unload every closed entry's GPU resources.
    for (auto &entry : m_images)
        if (!entry.open && entry.texture.is_loaded())
            entry.texture.unload(vk);

    // Remove the now-empty entries from the vector.
    std::erase_if(m_images, [](const ImageEntry &e) { return !e.open; });
}

/**
 * @brief Request that a specific image window receives ImGui keyboard focus.
 *
 * The focus is applied on the next call to draw_single_window() for that entry.
 *
 * @param id  The ImageEntry::id that should receive focus.
 */
void ImageViewerPanel::request_focus(int id) {
    m_requested_focus_id = id; // will be consumed by draw_single_window()
}

// ============================================================================
// Capacity queries
// ============================================================================

/**
 * @brief Returns the total number of ImageEntry objects (open and closing).
 */
int ImageViewerPanel::count() const {
    return static_cast<int>(m_images.size());
}

/**
 * @brief Build a snapshot of currently open images for use by other UI panels.
 * @return  Vector of lightweight OpenedFileInfo structs (no textures).
 */
std::vector<ImageViewerPanel::OpenedFileInfo> ImageViewerPanel::opened_files() const {
    std::vector<OpenedFileInfo> files;
    files.reserve(m_images.size());

    for (const auto &entry : m_images) {
        if (!entry.open)
            continue; // skip entries that are in the process of being evicted
        files.push_back(OpenedFileInfo{entry.title, entry.source, entry.kind, entry.id});
    }

    return files;
}

/**
 * @brief Return the ImTextureID for the already-loaded texture of the given source.
 *
 * Used by HistoryPreview to quickly reuse a texture that is already on the GPU
 * without re-downloading or re-decoding.
 *
 * @param source  The original file path or URL string to look up.
 * @return        Valid ImTextureID, or ImTextureID{} (zero) if not found.
 */
ImTextureID ImageViewerPanel::get_imgui_id_for_source(const std::string &source) const {
    for (const auto &entry : m_images) {
        if (entry.open && entry.source == source && entry.texture.is_loaded())
            return entry.texture.imgui_id(); // direct descriptor set handle
    }
    return ImTextureID{}; // not found — caller should fall back to loading
}

// ============================================================================
// View-menu integration
// ============================================================================

/**
 * @brief Emit one ImGui::MenuItem toggle per open image.
 *
 * Clicking a menu item sets entry.open = false, which causes evict_closed()
 * to free GPU resources at the start of the next frame.
 *
 * The caller must already be inside BeginMenu() / EndMenu().
 */
void ImageViewerPanel::build_view_menu_items() {
    for (auto &entry : m_images)
        ImGui::MenuItem(entry.title.c_str(), nullptr, &entry.open); // ImGui flips open on click
}

// ============================================================================
// Per-frame rendering
// ============================================================================

/**
 * @brief Render one ImGui window per open image.
 *
 * Call once per frame between ImGui::NewFrame() and ImGui::Render().
 * Closed entries are skipped; they are removed by evict_closed().
 */
void ImageViewerPanel::draw_windows() {
    m_context_menu.process_pending_save();
    for (auto &entry : m_images) {
        if (!entry.open)
            continue; // skip entries waiting to be evicted
        draw_single_window(entry);
    }
}

// ============================================================================
// Private — single window rendering
// ============================================================================

/**
 * @brief Render one image viewer ImGui window.
 *
 * Handles the full interaction loop for a single image:
 *   1. Window setup (title, initial size, aspect-ratio constraint, black bg).
 *   2. Input capture surface (InvisibleButton fills the canvas area).
 *   3. Drag routing — window-move (zoom==1 or Shift) vs image-pan (zoom>1).
 *   4. Zoom toward cursor on scroll wheel.
 *   5. Double-click to reset view.
 *   6. Offset clamping (image stays at least partially on screen).
 *   7. UV computation and Image() draw call.
 *   8. Zoom-percentage overlay (bottom-left corner).
 *
 * @param entry  The image entry to display.
 *               zoom and offset are mutated in-place by user interaction.
 */
void ImageViewerPanel::draw_single_window(ImageEntry &entry) {
    const float img_w = static_cast<float>(entry.texture.width);
    const float img_h = static_cast<float>(entry.texture.height);

    // Degenerate textures cannot be rendered.
    if (img_w <= 0.0f || img_h <= 0.0f)
        return;

    const float aspect_ratio = img_w / img_h; // used by the size constraint callback

    // ---- 1. Window configuration ----------------------------------------

    // Stable unique window id: title changes with file; ### id never changes.
    const std::string win_id = entry.title + "###img_" + std::to_string(entry.id);

    // On first appearance, open the window at a comfortable default size.
    ImGui::SetNextWindowSize(ImVec2(1000.f, 1000.f), ImGuiCond_FirstUseEver);

    // Apply a pending focus request (e.g. from the "Opened Files" panel).
    if (entry.id == m_requested_focus_id)
        ImGui::SetNextWindowFocus();

    // Lock the aspect ratio so resizing the width automatically adjusts the height.
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(200.f, 200.f), ImVec2(FLT_MAX, FLT_MAX),
        [](ImGuiSizeCallbackData *data) {
            // UserData carries a pointer to the aspect_ratio local — safe because
            // the lambda executes synchronously within SetNextWindowSizeConstraints.
            const float aspect = *static_cast<const float *>(data->UserData);
            data->DesiredSize.y = data->DesiredSize.x / aspect;
        },
        // static_cast is safe here: UserData is void* by the ImGui API contract.
        static_cast<void *>(const_cast<float *>(&aspect_ratio)));

    // Solid black background fills the letterbox area around the image.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));

    if (!ImGui::Begin(win_id.c_str(), &entry.open)) {
        // Window is collapsed or fully hidden — still consume the focus request.
        if (entry.id == m_requested_focus_id)
            m_requested_focus_id = -1;
        ImGui::End();
        ImGui::PopStyleColor(1);
        return;
    }

    // Focus request successfully applied — clear it so it fires only once.
    if (entry.id == m_requested_focus_id)
        m_requested_focus_id = -1;

    // Show a thumbnail preview when the mouse hovers over this entry's dock tab.
    if (const ImGuiWindow *window = ImGui::GetCurrentWindowRead()) {
        if ((window->DC.DockTabItemStatusFlags & ImGuiItemStatusFlags_HoveredRect) != 0)
            render_tab_hover_preview(entry);
    }

    // ---- 2. Interaction + rendering -------------------------------------

    const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();     // top-left of the content area
    const ImVec2 canvas_size = ImGui::GetContentRegionAvail(); // width × height in pixels
    const float base_scale = canvas_size.x / img_w;            // pixels-per-image-pixel at zoom 1

    handle_interactions(entry, canvas_pos, canvas_size, base_scale);
    clamp_view_to_bounds(entry, canvas_size, base_scale);
    render_image(entry, canvas_pos, canvas_size, base_scale);
    render_overlay(entry, canvas_pos, canvas_size);

    // Right-click context menu anywhere in the window.
    {
        WindowStateToml::ImageHistoryEntry hist{};
        hist.source = entry.source;
        hist.kind   = entry.kind;
        const auto result = m_context_menu.draw_for_window(
            hist, ("##img_ctx_" + std::to_string(entry.id)).c_str());
        if (result.erase)
            entry.open = false;
    }

    ImGui::End();
    ImGui::PopStyleColor(1);
}

/**
 * @brief Handle all mouse interactions for one image window.
 *
 * Drag routing — determines which mode applies each frame:
 *
 *   │ Condition                        │ Action                          │
 *   ├─────────────────────────────────┼─────────────────────────────────┤
 *   │ Shift + Left-drag (any zoom)    │ Move the ImGui window           │
 *   │ Left-drag, zoom == 1 (no shift) │ Move the ImGui window           │
 *   │ Left-drag, zoom >  1 (no shift) │ Pan the image within the canvas │
 *
 * The cursor icon is set to communicate the current mode:
 *   ImGuiMouseCursor_ResizeAll → window-move (four-directional arrow).
 *   ImGuiMouseCursor_Hand      → image-pan (grab hand).
 *
 * @param entry       Image entry whose zoom and offset are mutated.
 * @param pos         Top-left corner of the canvas in screen space.
 * @param size        Width × height of the canvas in pixels.
 * @param base_scale  Pixels-per-image-pixel at zoom 1.0.
 */
void ImageViewerPanel::handle_interactions(ImageEntry &entry,
                                           ImVec2 pos,
                                           ImVec2 size,
                                           float base_scale) {
    ImGui::InvisibleButton("##capture", size, ImGuiButtonFlags_MouseButtonLeft);
    const bool is_hovered = ImGui::IsItemHovered();
    const bool is_active = ImGui::IsItemActive();
    const float ppm = base_scale * entry.zoom;
    const bool shift_held = ImGui::GetIO().KeyShift;

    // ---- Zoom-to-cursor (scroll wheel) ------------------------------------
    if (is_hovered && ImGui::GetIO().MouseWheel != 0.0f)
        update_zoom(entry, pos, base_scale);

    // ---- Drag routing -------------------------------------------------------
    if (is_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        const bool window_move_mode = shift_held || (entry.zoom <= 1.0f);

        if (window_move_mode) {
            ImGuiWindow *window = ImGui::GetCurrentWindow();

            // When docked, move the entire dock node rather than the window.
            // SetWindowPos() is silently ignored on docked windows, so we must
            // reach into the dock node and reposition it directly.
            if (window && window->DockIsActive && window->DockNode) {
                ImGuiDockNode *node = window->DockNode;

                // Walk up to the root node — that is the floating host window
                // that the docking system actually positions on screen.
                while (node->ParentNode)
                    node = node->ParentNode;

                // The root node's host window is the one we can move freely.
                if (node->HostWindow) {
                    const ImVec2 new_pos = node->HostWindow->Pos + delta;
                    ImGui::SetWindowPos(node->HostWindow, new_pos);
                }
            } else {
                // Undocked — move the window directly as before.
                ImGui::SetWindowPos(ImGui::GetWindowPos() + delta);
            }

            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else {
            // Image-pan mode — unchanged.
            entry.offset.x -= delta.x / ppm;
            entry.offset.y -= delta.y / ppm;
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    } else if (is_hovered) {
        if (shift_held || entry.zoom <= 1.0f)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        else
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    // ---- Double-click reset -----------------------------------------------
    if (is_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        entry.zoom = 1.0f;
        entry.offset = {0.0f, 0.0f};
    }
}
/**
 * @brief Clamp the pan offset so the image cannot be scrolled completely off screen.
 *
 * At zoom 1.0 the offset is forced to zero (image fills the canvas exactly).
 * When zoomed in, the offset is clamped to the range
 *   [0, image_dimension − visible_dimension]
 * on each axis independently, which prevents panning past the image boundary.
 *
 * @param entry        Image entry whose offset is clamped in-place.
 * @param canvas_size  Current width × height of the canvas area.
 * @param base_scale   Pixels-per-image-pixel at zoom 1.0.
 */
void ImageViewerPanel::clamp_view_to_bounds(ImageEntry &entry,
                                            ImVec2 canvas_size,
                                            float base_scale) {
    const float img_w = static_cast<float>(entry.texture.width);
    const float img_h = static_cast<float>(entry.texture.height);
    const float ppm = base_scale * entry.zoom; // current pixels-per-image-pixel

    // How many image-space pixels are visible in the canvas at the current zoom.
    const float visible_w = canvas_size.x / ppm;
    const float visible_h = canvas_size.y / ppm;

    if (entry.zoom <= 1.0f) {
        // At fit-width scale the image exactly fills the canvas — no panning needed.
        entry.offset = {0.0f, 0.0f};
    } else {
        // Clamp each axis: offset cannot go below 0 (left/top edge of image)
        // or above image_size − visible_size (right/bottom edge).
        entry.offset.x = std::clamp(entry.offset.x, 0.0f, std::max(0.0f, img_w - visible_w));
        entry.offset.y = std::clamp(entry.offset.y, 0.0f, std::max(0.0f, img_h - visible_h));
    }
}

/**
 * @brief Update entry.zoom and entry.offset for a scroll-wheel zoom event.
 *
 * Keeps the image pixel under the cursor fixed in canvas-space as the zoom
 * changes, producing a "zoom toward cursor" effect.
 *
 * Derivation (see file-level comment for the full coordinate-space note):
 *   offset_new + cursor_canvas / ppm_new = offset_old + cursor_canvas / ppm_old
 *   → Δoffset = cursor_canvas * (1/ppm_old − 1/ppm_new)
 *
 * @param entry       Image entry whose zoom and offset are mutated.
 * @param canvas_pos  Top-left of the canvas in screen space.
 * @param base_scale  Pixels-per-image-pixel at zoom 1.0.
 */
void ImageViewerPanel::update_zoom(ImageEntry &entry,
                                   ImVec2 canvas_pos,
                                   float base_scale) {
    const float wheel = ImGui::GetIO().MouseWheel; // +1 per notch up, −1 per notch down
    const float old_zoom = entry.zoom;

    // Scale by 12 % per wheel notch, clamped to [1.0, 64.0].
    entry.zoom = std::clamp(old_zoom * (1.0f + wheel * 0.12f), 1.0f, 64.0f);

    // Cursor position relative to the canvas origin (canvas-space).
    const ImVec2 cursor_canvas = {
        ImGui::GetIO().MousePos.x - canvas_pos.x,
        ImGui::GetIO().MousePos.y - canvas_pos.y};

    const float old_ppm = base_scale * old_zoom;   // ppm before zoom change
    const float new_ppm = base_scale * entry.zoom; // ppm after  zoom change

    // Adjust offset so the image pixel under the cursor stays fixed.
    entry.offset.x += (cursor_canvas.x / old_ppm) - (cursor_canvas.x / new_ppm);
    entry.offset.y += (cursor_canvas.y / old_ppm) - (cursor_canvas.y / new_ppm);
}

/**
 * @brief Submit the image draw call using UV coordinates derived from the pan offset.
 *
 * UV mapping:
 *   uv0 = offset / image_size                 (top-left corner of the visible region)
 *   uv1 = (offset + visible_size) / image_size (bottom-right corner)
 *
 * @param entry       Image entry to render.
 * @param pos         Top-left of the canvas in screen space.
 * @param size        Width × height of the canvas.
 * @param base_scale  Pixels-per-image-pixel at zoom 1.0.
 */
void ImageViewerPanel::render_image(ImageEntry &entry,
                                    ImVec2 pos,
                                    ImVec2 size,
                                    float base_scale) {
    const float img_w = static_cast<float>(entry.texture.width);
    const float img_h = static_cast<float>(entry.texture.height);
    const float ppm = base_scale * entry.zoom;

    // How many image-space pixels are visible in the canvas at this zoom level.
    const float disp_w = size.x / ppm;
    const float disp_h = size.y / ppm;

    // Convert the image-space visible region to UV coordinates (0..1 range).
    const ImVec2 uv0 = {entry.offset.x / img_w, entry.offset.y / img_h};
    const ImVec2 uv1 = {(entry.offset.x + disp_w) / img_w, (entry.offset.y + disp_h) / img_h};

    // Position the draw cursor at the canvas origin and submit the draw call.
    ImGui::SetCursorScreenPos(pos);
    ImGui::Image(entry.texture.imgui_id(), size, uv0, uv1);
}

/**
 * @brief Draw the zoom-percentage text in the bottom-left corner of the canvas.
 *
 * Rendered after the image so it appears on top without needing a separate
 * draw-list layer.
 *
 * @param entry  Image entry providing the current zoom value.
 * @param pos    Top-left of the canvas in screen space.
 * @param size   Width × height of the canvas (used to find the bottom edge).
 */
void ImageViewerPanel::render_overlay(ImageEntry &entry, ImVec2 pos, ImVec2 size) {
    // 10 px from the left edge, 30 px from the bottom edge.
    ImGui::SetCursorScreenPos({pos.x + 10.0f, pos.y + size.y - 30.0f});
    ImGui::TextDisabled("%.0f %%", static_cast<double>(entry.zoom * 100.0f));
}

// ============================================================================
// Tooltip positioning helper
// ============================================================================

/**
 * @brief Compute a tooltip window position that stays fully within the display.
 *
 * Tries to place the window `offset` pixels to the right of and below the
 * cursor.  Flips each axis independently when the window would overflow the
 * display edge, then clamps to [0, display − window] as a final safety net.
 *
 * @param mouse        Current mouse position in display coordinates.
 * @param content_size Expected size of the tooltip content area (image dimensions).
 * @param offset       Gap in pixels between the cursor hot-spot and the window edge.
 * @return             Screen-space position for ImGui::SetNextWindowPos().
 */
ImVec2 ImageViewerPanel::clamp_tooltip_pos(ImVec2 mouse,
                                           ImVec2 content_size,
                                           float offset) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;  // full screen resolution
    const ImVec2 pad = ImGui::GetStyle().WindowPadding; // ImGui inner margin

    // Approximate total window height: content + title line + top/bottom padding.
    const float line_h = ImGui::GetTextLineHeightWithSpacing();
    const ImVec2 win_size = {
        content_size.x + pad.x * 2.0f,
        content_size.y + line_h + pad.y * 2.0f};

    // ---- Horizontal axis ---------------------------------------------------
    float x = mouse.x + offset;            // preferred: right of cursor
    if (x + win_size.x > display.x)        // would overflow right edge?
        x = mouse.x - win_size.x - offset; // flip: place to the left
    x = std::clamp(x, 0.0f, std::max(0.0f, display.x - win_size.x));

    // ---- Vertical axis -----------------------------------------------------
    float y = mouse.y + offset;            // preferred: below cursor
    if (y + win_size.y > display.y)        // would overflow bottom edge?
        y = mouse.y - win_size.y - offset; // flip: place above cursor
    y = std::clamp(y, 0.0f, std::max(0.0f, display.y - win_size.y));

    return {x, y};
}

// ============================================================================
// Dock-tab hover preview
// ============================================================================

/**
 * @brief Show a small thumbnail tooltip when the cursor hovers over this image's
 *        dock tab (not the image canvas itself).
 *
 * The tooltip is clamped so it never extends past any display edge.
 *
 * @param entry  The image entry to preview.
 */
void ImageViewerPanel::render_tab_hover_preview(const ImageEntry &entry) const {
    if (!entry.texture.is_loaded())
        return;

    // Scale the thumbnail to fit within hover_preview_size (upscaling allowed).
    const float src_w = static_cast<float>(entry.texture.width);
    const float src_h = static_cast<float>(entry.texture.height);
    const float max_w = ImageViewerPanel::hover_preview_size.x;
    const float max_h = ImageViewerPanel::hover_preview_size.y;
    const float scale = std::min(max_w / src_w, max_h / src_h);
    const float draw_w = src_w * scale;
    const float draw_h = src_h * scale;

    // Clamp the tooltip so it stays fully on-screen regardless of the tab position.
    const ImVec2 mouse = ImGui::GetMousePos();
    const ImVec2 tooltip_pos = clamp_tooltip_pos(mouse, {draw_w, draw_h});
    ImGui::SetNextWindowPos(tooltip_pos, ImGuiCond_Always);

    ImGui::BeginTooltip();
    ImGui::Text("%s", entry.title.c_str()); // window title as the tooltip header
    ImGui::Image(entry.texture.imgui_id(), ImVec2(draw_w, draw_h));
    ImGui::EndTooltip();
}