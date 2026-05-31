#pragma once

#include "pch.hpp"
#include "window_state_toml.hpp"

#include <SDL3/SDL_keycode.h>

class VideoContextMenu;
class HistoryPreview;
class VideoDownloader;
class vulkan_context;
class VideoHoverPreview;
class VulkanUploadContext;
class VideoUiWindow;
class VideoOsdOverlay;
class VideoSeekPreview;
struct VideoEntry;

/// Manages one or more mpv-backed video/GIF/audio windows rendered into
/// Vulkan textures via the libmpv software render API.
///
/// Each open media file gets its own mpv_handle so playback state is
/// independent.  Frame data is rendered by libmpv into a CPU pixel buffer
/// then uploaded to a per-entry VkImage every time the render-update
/// callback fires.
class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer &) = delete;
    VideoPlayer &operator=(const VideoPlayer &) = delete;

    /// Bind Vulkan context without starting worker threads.
    void bind_context(vulkan_context *vk);

    void setup(vulkan_context *vk);
    void shutdown();

    /// Open a local file.  Returns false on mpv initialisation failure.
    bool add_from_path(const std::filesystem::path &path);

    /// Open a local file while forcing a caller-supplied display title.
    bool add_from_path(const std::filesystem::path &path, const std::string &title);

    /// Open a local file while preserving a caller-supplied logical source key.
    bool add_from_path(const std::filesystem::path &path,
                       const std::string &title,
                       const std::string &logical_source);

    bool add_from_path(const std::filesystem::path &path,
                       const std::string &title,
                       const std::string &logical_source,
                       bool hwdec_enabled,
                       int resume_position_seconds,
                       const std::string &initial_osd_message);

    /// Stream a URL (mpv handles the network protocol).  Returns false on failure.
    bool add_from_url(const std::string &url, const std::string &title);
    bool add_from_url(const std::string &url,
                      const std::string &title,
                      bool hwdec_enabled,
                      int resume_position_seconds,
                      const std::string &initial_osd_message);

    /// Upload new frames to GPU and poll mpv events.  Call once per frame.
    void update_frames();

    /// Draw all open video windows.  Call once per frame inside an ImGui frame.
    void draw();

    /// Called when a background download finishes for @p url.
    /// If the corresponding entry is still open its seek-preview thread is
    /// started with the locally cached file.
    void notify_download_complete(const std::string &url,
                                  const std::filesystem::path &cached_path);

    /// Update an open entry to treat a saved file as its new canonical source.
    void replace_source_with_saved_file(const std::string &source,
                                        const std::filesystem::path &saved_path);

    /// Close the open video window for @p source.
    ///
    /// The entry is marked closed immediately and GPU/mpv resources are freed
    /// by the existing eviction path in draw().
    ///
    /// @return true if a matching open window was found and marked closed.
    bool close_window(const std::string &source);

    /// Close all open video windows.
    ///
    /// Entries are marked closed immediately and resources are released
    /// by the existing eviction path in draw().
    void close_all_windows();

    /// Returns true when at least one video window is open.
    [[nodiscard]] bool has_open_windows() const;

    /// Return the source strings of all currently open (non-closed) video windows.
    [[nodiscard]] std::vector<std::string> open_sources() const;


    /// Return the current playback frame descriptor for an already-open entry,
    /// or VK_NULL_HANDLE if no such entry exists.
    [[nodiscard]] VkDescriptorSet get_open_thumbnail(const std::string &source) const;

    /// Loads source into the shared hover preview and returns the latest
    /// thumbnail descriptor set.  Call once per frame while hovering.
    /// Returns VK_NULL_HANDLE until the first frame is ready.
    [[nodiscard]] VkDescriptorSet hover_thumbnail(const std::string &source);

    /// Register a hover on source to tick the dwell timer without fetching a frame.
    void notify_hover(const std::string &source);

    /// Save the current hover frame to a PNG file.
    /// Returns true on success; false if no valid frame has been rendered yet.
    bool save_hover_frame(const std::filesystem::path &path);

    /// Return true if the given path should be opened in the video player.
    static bool is_video_path(const std::filesystem::path &path);

    /// Return true if the given URL should be streamed in the video player.
    static bool is_video_url(const std::string &url);

    /// Attach a VideoDownloader so the player can show download progress in titles.
    void set_downloader(VideoDownloader *d);

    /// Stop and restart the hover-preview thread (useful when it gets stuck).
    void restart_hover_preview();

    /// True once when hover UI should close/reopen tooltip after hover thread recovery.
    [[nodiscard]] bool consume_hover_popup_reopen_request();

    /// True while the hover dwell delay has not yet elapsed for the given source.
    [[nodiscard]] bool is_hover_dwell_pending(const std::string &source) const;

    [[nodiscard]] bool can_toggle_hwdec(const std::string &source) const;
    [[nodiscard]] bool is_hwdec_enabled(const std::string &source) const;
    [[nodiscard]] int current_position_seconds(const std::string &source) const;
    [[nodiscard]] int persisted_position_seconds(const std::string &source) const;
    void set_resume_persist_min_duration_seconds(int seconds);
    [[nodiscard]] int resume_persist_min_duration_seconds() const;
    void toggle_hwdec(const std::string &source);
    void sync_history_state(std::vector<WindowStateToml::ImageHistoryEntry> &history) const;

    /// Set hwdec on all currently open entries (they reload immediately).
    void set_all_hwdec(bool enabled);

    /// Set loop on all currently open entries (applied immediately via mpv command).
    void set_all_loop(bool enabled);

    /// Restart all worker threads: hover preview + every open mpv entry (reload).
    void restart_all_threads();

    /// Handle an SDL multimedia key (SDLK_MEDIA_PLAY_PAUSE, SDLK_MEDIA_STOP, etc.)
    /// directed at the currently-active video entry.  No-op when no entry is active.
    void handle_media_key(SDL_Keycode key);

    /// Attach a VideoContextMenu for right-click menus on video windows.
    ///
    /// @param ctx     Context menu instance (lifetime must exceed VideoPlayer).
    /// @param lookup  Returns the history entry matching @p source, or nullptr.
    /// @param on_erase  Called when the user picks "Remove from History".
    void set_context_menu(
        VideoContextMenu *ctx,
        std::function<WindowStateToml::ImageHistoryEntry *(const std::string &)> lookup,
        std::function<void(const std::string &)> on_erase);

    /// Attach callbacks for the in-window "File" menu.
    ///
    /// @param on_open_image   Open the native file-open dialog.
    /// @param on_open_online  Open the URL input popup.
    /// @param on_open_recent  Open an item from history (source, kind).
    /// @param history         Provider that returns the current history list.
    /// @param preview         HistoryPreview for hover thumbnails (may be null).
    /// @param on_fix_videos   Toggle startup-video fixed flag for a video source.
    /// @param is_startup_videos_fixed Returns true when the source is fixed.
    void set_player_menu_callbacks(
        std::function<void()> on_open_image,
        std::function<void()> on_open_online,
        std::function<void(const std::string &, const std::string &)> on_open_recent,
        std::function<const std::vector<WindowStateToml::ImageHistoryEntry> &()> history,
        HistoryPreview *preview = nullptr,
        std::function<void(const std::string &)> on_fix_videos = nullptr,
        std::function<bool(const std::string &)> is_startup_videos_fixed = nullptr,
        std::function<bool()> on_get_app_fullscreen = nullptr,
        std::function<void(bool)> on_set_app_fullscreen = nullptr);


    /// Size used for hover/seek-preview thumbnails.
    static constexpr ImVec2 k_preview_size{320.0f, 180.0f};

private:
    bool ensure_setup();
    bool ensure_hover_setup();

    /// All per-video runtime state.
    

    static uint32_t find_memory_type(VkPhysicalDevice phys,
                                     uint32_t filter,
                                     VkMemoryPropertyFlags props);

    bool create_gpu_resources(VideoEntry &e);
    void destroy_gpu_resources(VideoEntry &e);
    bool upload_frame(VideoEntry &e);
    bool draw_window(VideoEntry &e, int idx);
    void poll_events(VideoEntry &e);
    void set_playback_state(int video_id, bool playing);
    void enforce_single_active_playback();
    [[nodiscard]] int current_position_seconds(const VideoEntry &entry) const;
    [[nodiscard]] int persisted_position_seconds(const VideoEntry &entry) const;

    // Shared hover thumbnail (one per VideoPlayer)
    std::unique_ptr<VideoHoverPreview> m_hover;
    std::unique_ptr<VulkanUploadContext> m_seek_uploader;
    std::unique_ptr<VideoUiWindow> m_ui_window;

    vulkan_context *m_vk;
    std::vector<std::unique_ptr<VideoEntry>> m_entries;
    int m_next_id;
    int m_active_video_id{-1};
    int m_resume_persist_min_duration_seconds;

    // Global defaults applied to all newly-opened videos.
    bool m_global_hwdec_enabled = false;
    bool m_global_loop_enabled  = false;
    bool m_initialized = false;
    bool m_hover_initialized = false;

    // Optional context menu (set via set_context_menu)
    VideoContextMenu *m_ctx_menu;
    std::function<WindowStateToml::ImageHistoryEntry *(const std::string &)> m_ctx_lookup;
    std::function<void(const std::string &)> m_ctx_on_erase;

    // In-window File menu callbacks (set via set_player_menu_callbacks)
    std::function<void()> m_on_open_image;
    std::function<void()> m_on_open_online;
    std::function<void(const std::string &, const std::string &)> m_on_open_recent;
    std::function<const std::vector<WindowStateToml::ImageHistoryEntry> &()> m_history_provider;
    HistoryPreview *m_history_preview;
    std::function<void(const std::string &)> m_on_fix_videos;
    std::function<bool(const std::string &)> m_is_startup_videos_fixed;
    std::function<bool()> m_on_get_app_fullscreen;
    std::function<void(bool)> m_on_set_app_fullscreen;
    VideoDownloader *m_downloader{nullptr};

    // Thread-safety guard: captured at setup(), checked in update_frames()/draw().
    std::thread::id m_main_thread_id;
};

