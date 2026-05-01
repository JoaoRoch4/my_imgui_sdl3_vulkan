/**
 * @file image_viewer_panel.cpp
 * @brief Implementation of ImageViewerPanel.
 *
 * Each open image is shown in its own resizable ImGui window.
 * The user can:
 *   - Scroll the mouse wheel to zoom toward / away from the cursor.
 *   - Left-drag to pan the image.
 *   - Double-click to reset zoom and pan.
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

#include <algorithm> /// std::clamp, std::erase_if
#include <cmath>     /// std::max
#include <string>    /// std::to_string

// ============================================================================
// Construction
// ============================================================================

/**
 * @brief Default constructor — starts with an empty image list.
 */
ImageViewerPanel::ImageViewerPanel()
    : m_images{}   /// No images open at startup.
    , m_next_id{0} /// IDs start at zero and increment monotonically.
{
}

// ============================================================================
// Public lifecycle
// ============================================================================

/**
 * @brief Load a texture from disk and register it as a new viewer window.
 *
 * Returns false (and does nothing) if:
 *   - The k_max_images cap is already reached.
 *   - VulkanTexture::load() fails to decode or upload the image.
 *
 * @param path  Path to the image file on disk.
 * @param vk    Active Vulkan context for GPU upload.
 * @return      true on success.
 */
bool ImageViewerPanel::add_from_path(const std::filesystem::path &path,
                                     vulkan_context &vk) {
    /// Reject early if we have hit the hard limit.
    if (static_cast<int>(m_images.size()) >= k_max_images)
        return false;

    /// Build the entry before moving it into m_images so that only a
    /// fully initialised, successfully loaded entry is ever committed.
    ImageEntry entry;

    /// Show only the filename part as the window title
    /// (e.g. "/home/user/photos/cat.png"  →  "cat.png").
    entry.title = path.filename().string();

    /// Assign a unique ID that will never be reused in this session.
    entry.id = m_next_id++;

    /// Mark the window as open immediately so it appears this frame.
    entry.open = true;

    /// Decode pixels from disk and upload an RGBA VkImage to the GPU.
    if (!entry.texture.load(path, vk))
        return false; /// Decoding failed; discard the entry.

    /// Transfer ownership into the managed list.
    m_images.push_back(std::move(entry));
    return true;
}

/**
 * @brief Load a texture from a temp file with a caller-supplied title.
 *
 * Designed for the URL download flow: the caller downloads a URL to a
 * temp path, hands the path here, then deletes the temp file afterward.
 *
 * @param tmp_path       Path to the temporary image file.
 * @param display_title  Title shown in the ImGui window title bar.
 * @param vk             Active Vulkan context for GPU upload.
 * @return               true on success.
 */
bool ImageViewerPanel::add_from_url_temp(const std::filesystem::path &tmp_path,
                                         const std::string &display_title,
                                         vulkan_context &vk) {
    /// Same cap guard as add_from_path.
    if (static_cast<int>(m_images.size()) >= k_max_images)
        return false;

    ImageEntry entry;
    entry.title = display_title; /// Caller derives this from the URL (e.g. filename part).
    entry.id = m_next_id++;
    entry.open = true;

    /// Load from the temp path — extension is preserved so the decoder
    /// (stb_image / libwebp) can detect the format correctly.
    if (!entry.texture.load(tmp_path, vk))
        return false;

    m_images.push_back(std::move(entry));
    return true;
}

/**
 * @brief Destroy all GPU resources owned by this panel.
 *
 * Must be called before ImGui_ImplVulkan_Shutdown() and before the
 * VkDevice is destroyed.  Safe to call on an already-empty panel.
 *
 * @param vk  Active Vulkan context for resource destruction.
 */
void ImageViewerPanel::shutdown(vulkan_context &vk) {
    /// Unload every entry regardless of its open state.
    for (auto &entry : m_images)
        if (entry.texture.is_loaded())
            entry.texture.unload(vk);

    /// Clear so subsequent calls are harmless no-ops.
    m_images.clear();
}

// ============================================================================
// Capacity queries
// ============================================================================

/**
 * @brief Returns true when the viewer already holds k_max_images images.
 *
 * Used by MainMenuBar to disable "Open Image" menu items and to skip
 * pending file / URL processing without attempting a doomed load.
 */
bool ImageViewerPanel::is_at_capacity() const {
    return static_cast<int>(m_images.size()) >= k_max_images;
}

/**
 * @brief Returns the number of currently managed ImageEntry objects.
 */
int ImageViewerPanel::count() const {
    return static_cast<int>(m_images.size());
}

// ============================================================================
// View-menu integration
// ============================================================================

/**
 * @brief Emit one ImGui::MenuItem toggle per open image.
 *
 * Toggling an item sets entry.open = false, which causes evict_closed()
 * to free the GPU resources at the start of the next frame.
 *
 * The caller must already be inside BeginMenu() / EndMenu().
 */
void ImageViewerPanel::build_view_menu_items() {
    for (auto &entry : m_images) {
        /// MenuItem with a bool* pSelected — ImGui flips it on click.
        ImGui::MenuItem(entry.title.c_str(), nullptr, &entry.open);
    }
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
    for (auto &entry : m_images) {
        // Only draw if the user hasn't clicked the [X] button
        if (!entry.open)
            continue;

        draw_single_window(entry); // Renders a unique window using entry.id
    }
}
// ============================================================================
// Private — single window rendering
// ============================================================================

/**
 * @brief Render one image viewer ImGui window.
 *
 * Handles the full interaction loop for a single image:
 *   1. Window setup (title, size, black background).
 *   2. Input capture surface (InvisibleButton).
 *   3. Pan on left-drag.
 *   4. Zoom toward cursor on scroll wheel.
 *   5. Double-click to reset view.
 *   6. Offset clamping (image stays partially on screen).
 *   7. UV computation and Image() draw call.
 *   8. Zoom-percentage overlay (bottom-left corner).
 *
 * @param entry  The image entry to display.
 *               zoom and offset are mutated in-place by user interaction.
 */
// image_viewer_panel.cpp
void ImageViewerPanel::draw_single_window(ImageEntry &entry) {
    const float img_w = static_cast<float>(entry.texture.width);
    const float img_h = static_cast<float>(entry.texture.height);

    if (img_w <= 0.0f || img_h <= 0.0f)
        return;
    const float aspect_ratio = img_w / img_h;

    // 1. Window Configuration
    const std::string win_id = entry.title + "###img_" + std::to_string(entry.id);
    ImGui::SetNextWindowSize(ImVec2(1000.f, 1000.f), ImGuiCond_FirstUseEver);

    ImGui::SetNextWindowSizeConstraints(
        ImVec2(200.f, 200.f), ImVec2(FLT_MAX, FLT_MAX),
        [](ImGuiSizeCallbackData *data) {
            float aspect = *(float *)data->UserData;
            data->DesiredSize.y = data->DesiredSize.x / aspect;
        },
        (void *)&aspect_ratio);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    if (!ImGui::Begin(win_id.c_str(), &entry.open)) {
        ImGui::End();
        ImGui::PopStyleColor(1);
        return;
    }

    // 2. Logic Execution
    const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    const float base_scale = canvas_size.x / img_w;

    handle_interactions(entry, canvas_pos, canvas_size, base_scale);
    clamp_view_to_bounds(entry, canvas_size, base_scale); // <-- New "Wall" Logic
    render_image(entry, canvas_pos, canvas_size, base_scale);
    render_overlay(entry, canvas_pos, canvas_size);

    ImGui::End();
    ImGui::PopStyleColor(1);
}
void ImageViewerPanel::handle_interactions(ImageEntry &entry, ImVec2 pos, ImVec2 size, float base_scale) {
    ImGui::InvisibleButton("##capture", size, ImGuiButtonFlags_MouseButtonLeft);
    const bool is_hovered = ImGui::IsItemHovered();
    const bool is_active = ImGui::IsItemActive();
    const float ppm = base_scale * entry.zoom;

    // Zoom-to-Cursor
    if (is_hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        update_zoom(entry, pos, base_scale);
    }

    // Shift + Left-drag: move the ImGui window itself
    if (is_active && ImGui::GetIO().KeyShift && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        ImGui::SetWindowPos(ImGui::GetWindowPos() + delta);
    }
    // Panning (Disabled if at 'wall' or not zoomed)
    else if (is_active && entry.zoom > 1.0f && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        entry.offset.x -= delta.x / ppm;
        entry.offset.y -= delta.y / ppm;
    }

    // Reset on Double Click
    if (is_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        entry.zoom = 1.0f;
        entry.offset = {0.0f, 0.0f};
    }
}
void ImageViewerPanel::clamp_view_to_bounds(ImageEntry &entry, ImVec2 canvas_size, float base_scale) {
    const float img_w = static_cast<float>(entry.texture.width);
    const float img_h = static_cast<float>(entry.texture.height);
    const float ppm = base_scale * entry.zoom;

    // Calculate how much of the image is visible in pixels
    const float visible_w = canvas_size.x / ppm;
    const float visible_h = canvas_size.y / ppm;

    if (entry.zoom <= 1.0f) {
        // Center/Reset if zoomed out
        entry.offset = {0.0f, 0.0f};
    } else {
        // Clamp the offset so you cannot pan past the edges of the image
        // Min offset is 0 (Top-Left), Max offset is ImageSize - VisibleSize (Bottom-Right)
        entry.offset.x = std::clamp(entry.offset.x, 0.0f, std::max(0.0f, img_w - visible_w));
        entry.offset.y = std::clamp(entry.offset.y, 0.0f, std::max(0.0f, img_h - visible_h));
    }
}
void ImageViewerPanel::update_zoom(ImageEntry &entry, ImVec2 canvas_pos, float base_scale) {
    const float wheel = ImGui::GetIO().MouseWheel;
    const float old_zoom = entry.zoom;
    entry.zoom = std::clamp(old_zoom * (1.0f + wheel * 0.12f), 1.0f, 64.0f); // Min zoom 1.0 to fit bounds

    const ImVec2 mouse_rel = {
        ImGui::GetIO().MousePos.x - canvas_pos.x,
        ImGui::GetIO().MousePos.y - canvas_pos.y};

    const float old_ppm = base_scale * old_zoom;
    const float new_ppm = base_scale * entry.zoom;

    entry.offset.x += (mouse_rel.x / old_ppm) - (mouse_rel.x / new_ppm);
    entry.offset.y += (mouse_rel.y / old_ppm) - (mouse_rel.y / new_ppm);
}

void ImageViewerPanel::render_image(ImageEntry &entry, ImVec2 pos, ImVec2 size, float base_scale) {
    const float img_w = static_cast<float>(entry.texture.width);
    const float img_h = static_cast<float>(entry.texture.height);
    const float ppm = base_scale * entry.zoom;

    const float disp_w = size.x / ppm;
    const float disp_h = size.y / ppm;

    // UVs now strictly follow the clamped offset
    const ImVec2 uv0 = {entry.offset.x / img_w, entry.offset.y / img_h};
    const ImVec2 uv1 = {(entry.offset.x + disp_w) / img_w, (entry.offset.y + disp_h) / img_h};

    ImGui::SetCursorScreenPos(pos);
    ImGui::Image(entry.texture.imgui_id(), size, uv0, uv1);
}

void ImageViewerPanel::render_overlay(ImageEntry &entry, ImVec2 pos, ImVec2 size) {
    ImGui::SetCursorScreenPos({pos.x + 10.0f, pos.y + size.y - 30.0f});
    ImGui::TextDisabled("%.0f %%", static_cast<double>(entry.zoom * 100.0f));
}
