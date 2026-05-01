#pragma once

#include "imgui.h"
#include "vulkan_context.hpp"
#include "vulkan_texture.hpp"

#include <filesystem>
#include <string>
#include <vector>

/**
 * Manages a collection of ImGui image-viewer windows.
 * Each window supports zoom-to-cursor (scroll wheel) and pan (left-drag).
 */
class ImageViewerPanel {
public:
    ImageViewerPanel();
    ~ImageViewerPanel() = default;

    ImageViewerPanel(const ImageViewerPanel &) = delete;
    ImageViewerPanel &operator=(const ImageViewerPanel &) = delete;
    ImageViewerPanel(ImageViewerPanel &&) = delete;
    ImageViewerPanel &operator=(ImageViewerPanel &&) = delete;

    /// Unload GPU resources for every entry whose open flag is false.
    /// Call at the START of each frame, after the frame fence signals.
    void evict_closed(vulkan_context &vk);

    /// Load a texture from disk and open a new viewer window.
    /// Returns false if the cap is reached or decoding fails.
    [[nodiscard]] bool add_from_path(const std::filesystem::path &path,
                                     vulkan_context &vk);

    /// Load a texture from a temp file with a caller-supplied display title.
    /// The caller is responsible for deleting tmp_path afterward.
    /// Returns false if the cap is reached or decoding fails.
    [[nodiscard]] bool add_from_url_temp(const std::filesystem::path &tmp_path,
                                         const std::string &display_title,
                                         vulkan_context &vk);

    /// Render one ImGui window per open image.
    /// Call between NewFrame() and Render().
    void draw_windows();

    /// Emit one MenuItem toggle per open image.
    /// Caller must already be inside BeginMenu() / EndMenu().
    void build_view_menu_items();

    /// Destroy all GPU resources. Must be called before ImGui_ImplVulkan_Shutdown.
    void shutdown(vulkan_context &vk);

    [[nodiscard]] bool is_at_capacity() const; ///< True when k_max_images is reached.
    [[nodiscard]] int count() const;           ///< Number of currently managed entries.

private:
    /// Per-image runtime state.
    struct ImageEntry {
        VulkanTexture texture;     ///< GPU-side RGBA texture.
        std::string title;         ///< Window title bar text.
        int id{0};                 ///< Stable monotonic ImGui ID.
        bool open{true};           ///< false → evict at next evict_closed().
        float zoom{1.0f};          ///< 1.0 = fit image width to canvas.
        ImVec2 offset{0.0f, 0.0f}; ///< Pan origin in image-space pixels.
    };

    static constexpr int k_max_images = 8; ///< Hard VRAM cap.

    void draw_single_window(ImageEntry &entry);
    void handle_interactions(ImageEntry &entry, ImVec2 pos, ImVec2 size, float base_scale);
    void update_zoom(ImageEntry &entry, ImVec2 canvas_pos, float base_scale);
    void clamp_view_to_bounds(ImageEntry &entry, ImVec2 canvas_size, float base_scale);
    void render_image(ImageEntry &entry, ImVec2 pos, ImVec2 size, float base_scale);
    void render_overlay(ImageEntry &entry, ImVec2 pos, ImVec2 size);

    std::vector<ImageEntry> m_images; ///< All open (or closing) image entries.
    int m_next_id;                    ///< Monotonically increasing ID counter.
};