#pragma once

#include "pch.hpp"

#include "image_context_menu.hpp"
#include "vulkan_context.hpp"
#include "vulkan_texture.hpp"

struct SDL_Window;


/**
 * Manages a collection of ImGui image-viewer windows.
 *
 * Each window supports:
 *   - Scroll wheel   → zoom toward / away from the cursor.
 *   - Left-drag      → pan the image when zoomed in (zoom > 1).
 *   - Left-drag      → move the ImGui window when not zoomed in (zoom == 1).
 *   - Shift+drag     → always moves the ImGui window regardless of zoom level.
 *   - Double-click   → reset zoom and pan to default.
 */
class ImageViewerPanel {
public:
    struct OpenedFileInfo {
        std::string title;  ///< Display name shown in the window title bar.
        std::string source; ///< Original file path or URL.
        std::string kind;   ///< "file" | "url"
        int id;             ///< Stable monotonic identifier.
    };

    ImageViewerPanel();
    ~ImageViewerPanel() = default;

    /// Maximum bounding box for the tab hover thumbnail (never upscales).
    static inline ImVec2 hover_preview_size{280.0f, 180.0f};

    /// Store the SDL window used to parent save-file dialogs.
    void setup(SDL_Window *window);

    /// Register a callback invoked after a successful image save.
    void set_on_save_success(
        std::function<void(const std::string &, const std::filesystem::path &)> cb);

    ImageViewerPanel(const ImageViewerPanel &)            = delete;
    ImageViewerPanel &operator=(const ImageViewerPanel &) = delete;
    ImageViewerPanel(ImageViewerPanel &&)                 = delete;
    ImageViewerPanel &operator=(ImageViewerPanel &&)      = delete;

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
                                         const std::string            &display_title,
                                         const std::string            &source_url,
                                         vulkan_context               &vk);

    /// Render one ImGui window per open image.  Call between NewFrame() and Render().
    void draw_windows();

    /// Emit one MenuItem toggle per open image.
    /// Caller must already be inside BeginMenu() / EndMenu().
    void build_view_menu_items();

    /// Destroy all GPU resources. Must be called before ImGui_ImplVulkan_Shutdown.
    void shutdown(vulkan_context &vk);

    /// Set the image entry (by id) that should receive keyboard focus next frame.
    void request_focus(int id);

    [[nodiscard]] int count() const; ///< Number of currently managed entries.
    [[nodiscard]] std::vector<OpenedFileInfo> opened_files() const;

    /// Return the ImTextureID for the already-loaded texture of the given source,
    /// or 0 if not currently open.
    [[nodiscard]] ImTextureID get_imgui_id_for_source(const std::string &source) const;

private:
    /// Per-image runtime state.
    struct ImageEntry {
        VulkanTexture texture;          ///< GPU-side RGBA texture.
        std::string   title;            ///< Window title bar text.
        std::string   source;           ///< Original source path/URL for history sync.
        std::string   kind;             ///< "file" or "url".
        int           id{0};            ///< Stable monotonic ImGui ID.
        bool          open{true};       ///< false → evict at next evict_closed().
        float         zoom{1.0f};       ///< 1.0 = fit image width to canvas.
        ImVec2        offset{0.0f, 0.0f}; ///< Pan origin in image-space pixels.
    };

    // -------------------------------------------------------------------------
    // Rendering helpers
    // -------------------------------------------------------------------------

    void draw_single_window(ImageEntry &entry);
    void handle_interactions(ImageEntry &entry, ImVec2 pos, ImVec2 size, float base_scale);
    void update_zoom(ImageEntry &entry, ImVec2 canvas_pos, float base_scale);
    void clamp_view_to_bounds(ImageEntry &entry, ImVec2 canvas_size, float base_scale);
    void render_image(ImageEntry &entry, ImVec2 pos, ImVec2 size, float base_scale);
    void render_overlay(ImageEntry &entry, ImVec2 pos, ImVec2 size);
    void render_tab_hover_preview(const ImageEntry &entry) const;

    /// Clamp a tooltip window so it never extends past any display edge.
    [[nodiscard]] static ImVec2 clamp_tooltip_pos(ImVec2 mouse,
                                                  ImVec2 content_size,
                                                  float  offset = 24.0f);

    // -------------------------------------------------------------------------
    // Members
    // -------------------------------------------------------------------------

    std::vector<ImageEntry> m_images;           ///< All open (or closing) image entries.
    int                     m_next_id;           ///< Monotonically increasing ID counter.
    int                     m_requested_focus_id; ///< Entry id that wants focus, or -1.
    ImageContextMenu        m_context_menu;       ///< Right-click context menu for open images.
};