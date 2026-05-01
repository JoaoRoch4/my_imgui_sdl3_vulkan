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
    struct OpenedFileInfo {
        std::string title;
        std::string source;
        std::string kind;
        int id;
    };

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
                                         const std::string &source_url,
                                         vulkan_context &vk);

    /// Render one ImGui window per open image.
    /// Call between NewFrame() and Render().
    void draw_windows();

    /// Emit one MenuItem toggle per open image.
    /// Caller must already be inside BeginMenu() / EndMenu().
    void build_view_menu_items();

    /// Destroy all GPU resources. Must be called before ImGui_ImplVulkan_Shutdown.
    void shutdown(vulkan_context &vk);
    void request_focus(int id);

    [[nodiscard]] int count() const; ///< Number of currently managed entries.
    [[nodiscard]] std::vector<OpenedFileInfo> opened_files() const;

    /// Return the ImTextureID for the already-loaded texture of the given source,
    /// or 0 if not currently open.
    [[nodiscard]] ImTextureID get_imgui_id_for_source(const std::string &source) const;

private:
    /// Per-image runtime state.
    struct ImageEntry {
        VulkanTexture texture;     ///< GPU-side RGBA texture.
        std::string title;         ///< Window title bar text.
        std::string source;        ///< Original source path/URL for history sync.
        std::string kind;          ///< "file" or "url".
        int id{0};                 ///< Stable monotonic ImGui ID.
        bool open{true};           ///< false → evict at next evict_closed().
        float zoom{1.0f};          ///< 1.0 = fit image width to canvas.
        ImVec2 offset{0.0f, 0.0f}; ///< Pan origin in image-space pixels.
    };

    void draw_single_window(ImageEntry &entry);
    void handle_interactions(ImageEntry &entry, ImVec2 pos, ImVec2 size, float base_scale);
    void update_zoom(ImageEntry &entry, ImVec2 canvas_pos, float base_scale);
    void clamp_view_to_bounds(ImageEntry &entry, ImVec2 canvas_size, float base_scale);
    void render_image(ImageEntry &entry, ImVec2 pos, ImVec2 size, float base_scale);
    void render_overlay(ImageEntry &entry, ImVec2 pos, ImVec2 size);
    void render_tab_hover_preview(const ImageEntry &entry) const;

    std::vector<ImageEntry> m_images; ///< All open (or closing) image entries.
    int m_next_id;                    ///< Monotonically increasing ID counter.
    int m_requested_focus_id;
};