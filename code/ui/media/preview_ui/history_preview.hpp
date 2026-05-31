#pragma once

#include "pch.hpp"

#include "vulkan_context.hpp"
#include "vulkan_texture.hpp"
#include "window_state_toml.hpp"


class VideoPlayer;
class VideoPlayerPlacebo;
class ImageViewerPanel;

class HistoryPreview {
public:
    HistoryPreview();
    ~HistoryPreview();

    HistoryPreview(const HistoryPreview &) = delete;
    HistoryPreview &operator=(const HistoryPreview &) = delete;

    void setup(vulkan_context *vk,
               VideoPlayer      *vp     = nullptr,
               ImageViewerPanel *viewer = nullptr,
               VideoPlayerPlacebo *vpp   = nullptr,
               bool use_video_player_placebo = false);
    void set_use_video_player_placebo(bool enabled);
    void shutdown();

    void set_thumb_dir(const std::filesystem::path &dir);
    void draw_for_hover(WindowStateToml::ImageHistoryEntry &hentry);

private:
    // -------------------------------------------------------------------------
    // Worker job types
    // -------------------------------------------------------------------------

    struct Job {
        std::string source; ///< Absolute path or URL.
        std::string kind;   ///< "file" | "url"
    };

    struct JobResult {
        std::string        source;       ///< Matches the originating Job::source.
        std::filesystem::path path;      ///< Local path to the decoded or downloaded image.
        bool               is_temp_file; ///< True → caller must delete path after use.
        bool               ok;           ///< True → path is valid and ready to upload.
    };

    // -------------------------------------------------------------------------
    // Preview positioning helper
    // -------------------------------------------------------------------------

    /**
     * @brief Compute a tooltip position that keeps the window fully on-screen.
     *
     * Tries to place the window to the right of and below the cursor.  Flips
     * each axis independently when the window would overflow the display edge.
     *
     * @param mouse        Current mouse position in display coordinates.
     * @param preview_size Expected size of the tooltip window content area.
     * @param offset       Pixel gap between the cursor hot-spot and the window edge.
     * @return             Screen-space position to pass to SetNextWindowPos().
     */
    [[nodiscard]] static ImVec2 clamp_tooltip_pos(ImVec2 mouse,
                                                  ImVec2 preview_size,
                                                  float  offset = 24.0f);

    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    void request_preview(const WindowStateToml::ImageHistoryEntry &hentry);
    void clear_active_preview();
    void apply_ready_result(const WindowStateToml::ImageHistoryEntry &hentry);
    void start_worker_thread();
    void stop_worker_thread(bool unregister_watch);
    void worker_loop(std::stop_token stoken);

    // -------------------------------------------------------------------------
    // Members
    // -------------------------------------------------------------------------

    vulkan_context   *m_vk;           ///< Active Vulkan device.
    VideoPlayer      *m_video_player; ///< Used for video thumbnail queries.
    VideoPlayerPlacebo *m_video_player_placebo; ///< Optional VPP for video thumbnail queries.
    ImageViewerPanel *m_viewer;       ///< Used for already-open image texture look-up.
    bool m_use_video_player_placebo;  ///< Use VPP instead of VP for thumbnail operations.

    std::jthread              m_worker;           ///< Background download / path-check thread.
    std::atomic<uint64_t>     m_worker_watch_id;  ///< ThreadOverwatch registration id.
    std::mutex                m_mutex;
    std::condition_variable   m_cv;

    bool      m_has_pending_job;  ///< A job is waiting for the worker thread.
    Job       m_pending_job;      ///< The queued job (source + kind).
    bool      m_has_ready_result; ///< The worker has finished and left a result.
    JobResult m_ready_result;     ///< Result produced by the worker thread.

    VulkanTexture         m_active_texture;   ///< GPU texture for the currently shown preview.
    std::string           m_active_source;    ///< Source string that m_active_texture was loaded from.
    std::filesystem::path m_active_temp_path; ///< Temp file to delete when the preview changes.
    std::filesystem::path m_thumb_dir;        ///< Directory where video thumbnail PNGs are cached.
    std::unordered_set<std::string> m_saved_video_thumbnail_sources; ///< Sources with persisted video thumbnail PNG.
};