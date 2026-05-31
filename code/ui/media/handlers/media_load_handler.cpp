/**
 * @file media_load_handler.cpp
 * @brief Routes pending file paths and URLs to the correct media consumer.
 *
 * All routing logic that previously lived inline in MainMenuBar::Build() steps 2
 * and 3 is now centralised here:
 *
 *   process_pending_paths()  — local files from the file dialog or clipboard.
 *   process_pending_urls()   — http/https URLs from the "Open Online" popup.
 *   drain_bulk_queue()       — one validated path per frame from the bulk worker.
 *   restore_from_history()   — rebuilds video windows from history at startup.
 *
 * The class holds only non-owning pointers; all lifetimes are managed by
 * MainMenuBar.
 */

#include "pch.hpp"

#include "media_load_handler.hpp"

#include "bulk_image_open_queue.hpp"
#include "image_downloader.hpp"
#include "Image_viewer_panel.hpp"
#include "media_history_manager.hpp"
#include "opened_files_window.hpp"
#include "video_downloader.hpp"
#include "video_player.hpp"
#include "video_player_placebo.hpp"
#include "vulkan_context.hpp"



// ============================================================================
// Constructor
// ============================================================================

/**
 * @brief Default constructor — all pointer members are null-initialised.
 *
 * setup() must be called with valid pointers before any process_* method.
 */
MediaLoadHandler::MediaLoadHandler()
    : m_viewer{nullptr}       // image rendering target — set in setup()
    , m_video_player{nullptr} // video rendering target — set in setup()
    , m_video_player_placebo{nullptr}
    , m_bulk_queue{nullptr}   // off-thread path validator — set in setup()
    , m_history{nullptr}      // history push + persist — set in setup()
    , m_downloader{nullptr}   // background video cache — set in setup()
    , m_files_window{nullptr} // companion panel for history sync — set in setup()
    , m_vk{nullptr}           // Vulkan device for texture upload — set in setup()
    , m_use_video_player_placebo{false}
{
}

// ============================================================================
// Setup
// ============================================================================

/**
 * @brief Store non-owning pointers to every subsystem this handler routes into.
 *
 * All arguments must remain valid for the lifetime of this object; ownership
 * stays with MainMenuBar.  Call this once from MainMenuBar::Setup() after all
 * subsystems have been constructed and individually initialised.
 *
 * @param viewer       ImageViewerPanel that receives decoded image frames.
 * @param video_player VideoPlayer that receives video paths and URLs.
 * @param bulk_queue   Off-thread validator for large file-dialog batches.
 * @param history      MediaHistoryManager — receives push() calls on success.
 * @param downloader   VideoDownloader — checked for cached files before streaming.
 * @param files_window OpenedFilesWindow companion panel, passed into history.push().
 * @param vk           Active Vulkan context needed by ImageViewerPanel::add_from_path().
 */
void MediaLoadHandler::setup(ImageViewerPanel    *viewer,
                             VideoPlayer         *video_player,
                             VideoPlayerPlacebo  *video_player_placebo,
                             BulkImageOpenQueue  *bulk_queue,
                             MediaHistoryManager *history,
                             VideoDownloader     *downloader,
                             OpenedFilesWindow   *files_window,
                             vulkan_context      *vk,
                             bool                 use_video_player_placebo)
{
    m_viewer       = viewer;       // image rendering target
    m_video_player = video_player; // video rendering target
    m_video_player_placebo = video_player_placebo;
    m_bulk_queue   = bulk_queue;   // background path validator
    m_history      = history;      // history list + persistence
    m_downloader   = downloader;   // video cache / background downloader
    m_files_window = files_window; // companion UI for history sync
    m_vk           = vk;           // Vulkan device for texture creation
    m_use_video_player_placebo = use_video_player_placebo;
}

// ============================================================================
// Public processing — called once per frame from MainMenuBar::Build()
// ============================================================================

/**
 * @brief Route a batch of local file paths to the correct media consumer.
 *
 * Decision tree:
 *   - Batch of more than one path → videos open immediately; images are sent
 *     to BulkImageOpenQueue for off-thread existence validation, then opened
 *     one-per-frame by drain_bulk_queue().
 *   - Single path → opened directly so the user sees immediate feedback.
 *
 * @param paths  Absolute local file-system paths (may mix images and videos).
 */
void MediaLoadHandler::process_pending_paths(const std::vector<std::string>& paths)
{
    const auto is_video_path = [this](const std::string &path) {
        return m_use_video_player_placebo ? VideoPlayerPlacebo::is_video_path(path)
                                          : VideoPlayer::is_video_path(path);
    };
    const auto add_from_path = [this](const std::filesystem::path &path) {
        return m_use_video_player_placebo ? m_video_player_placebo->add_from_path(path)
                                          : m_video_player->add_from_path(path);
    };

    // Nothing to do with an empty batch or without a valid Vulkan context.
    if (paths.empty() || !m_vk)
        return;

    if (paths.size() > 1) {
        // ---- Large batch ------------------------------------------------
        // Separate the batch: open videos immediately, queue images for the
        // background validator to avoid blocking the main thread on large
        // directories.
        std::vector<std::string> image_paths;
        image_paths.reserve(paths.size()); // avoid repeated heap reallocations

        for (const auto &path : paths) {
            if (is_video_path(path)) {
                // Route directly to the player; it queues internally.
                add_from_path(path);

                // Record in history using only the filename as the title.
                m_history->push(path, "file",
                                std::filesystem::path(path).filename().string(),
                                *m_files_window);
            } else {
                // Accumulate image paths for the off-thread bulk validator.
                image_paths.push_back(path);
            }
        }

        // Hand the collected image paths to the background worker.
        if (!image_paths.empty())
            m_bulk_queue->enqueue_batch(image_paths);

    } else {
        // ---- Single path ------------------------------------------------
        // Open directly so the user sees immediate visual feedback.
        const std::string &path = paths.front(); // only element in the vector
        const std::string title = std::filesystem::path(path).filename().string();

        if (is_video_path(path)) {
            // Single video — open in the player and push to history on success.
            if (add_from_path(path))
                m_history->push(path, "file", title, *m_files_window);
        } else {
            // Single image — upload to the viewer panel and push to history.
            if (m_viewer->add_from_path(path, *m_vk))
                m_history->push(path, "file", title, *m_files_window);
        }
    }
}

/**
 * @brief Download or stream pending URLs and hand them to the correct consumer.
 *
 * Decision tree per URL:
 *   - Video URL → played directly (streaming); simultaneously enqueued in
 *     VideoDownloader for background caching to local disk.
 *   - Image URL → downloaded synchronously to a temp file; decoded by the
 *     image viewer panel; temp file deleted immediately after.
 *
 * @param urls  Full http/https URL strings collected from the "Open Online" popup.
 */
void MediaLoadHandler::process_pending_urls(const std::vector<std::string>& urls)
{
    const auto is_video_url = [this](const std::string &url) {
        return m_use_video_player_placebo ? VideoPlayerPlacebo::is_video_url(url)
                                          : VideoPlayer::is_video_url(url);
    };
    const auto add_from_path = [this](const std::filesystem::path &path,
                                      const std::string &title,
                                      const std::string &source) {
        return m_use_video_player_placebo ? m_video_player_placebo->add_from_path(path, title, source)
                                          : m_video_player->add_from_path(path, title, source);
    };
    const auto add_from_url = [this](const std::string &url,
                                     const std::string &title) {
        return m_use_video_player_placebo ? m_video_player_placebo->add_from_url(url, title)
                                          : m_video_player->add_from_url(url, title);
    };

    // Nothing to do with an empty list or without a valid Vulkan context.
    if (urls.empty() || !m_vk)
        return;

    for (const auto &url : urls) {
        // ---- Video URL --------------------------------------------------
        if (is_video_url(url)) {
            // Derive a human-readable name from the URL's path/query components.
            const std::string title = ImageDownloader::title_from_url(url);

            // Check the background cache for a locally downloaded copy.
            const auto cached = m_downloader->get_or_enqueue(url);

            if (cached) {
                // A cached file exists — play from disk (faster and seekable).
                if (add_from_path(*cached, title, url)) {
                    m_history->push(url, "url", title, *m_files_window);
                    // Write the cache path into the history entry just pushed.
                    auto &entries = m_history->entries();
                    if (!entries.empty())
                        entries.front().cached_path = cached->string();
                }
            } else {
                // No cache yet — stream directly from the URL.
                if (add_from_url(url, title))
                    m_history->push(url, "url", title, *m_files_window);
            }
            continue; // done with this URL; proceed to the next
        }

        // ---- Image URL --------------------------------------------------

        // Blocking download: writes image bytes to a uniquely-named temp file
        // whose extension matches the image format (e.g. .jpg, .png, .webp).
        const std::filesystem::path tmp = ImageDownloader::download_to_temp(url);

        // If the download failed (network error, bad URL), skip silently.
        if (tmp.empty())
            continue;

        // Derive the display title from the URL's filename/query component.
        const std::string title = ImageDownloader::title_from_url(url);

        // Hand the temp file to ImageViewerPanel — it decodes and uploads.
        if (m_viewer->add_from_url_temp(tmp, title, url, *m_vk))
            m_history->push(url, "url", title, *m_files_window);

        // Remove the temp file regardless of whether decoding succeeded.
        std::filesystem::remove(tmp);
    }
}

/**
 * @brief Pop at most one validated path from BulkImageOpenQueue and open it.
 *
 * Called once per frame to avoid uploading many textures in the same frame,
 * which would exhaust the Vulkan descriptor pool and spike frame time.
 */
void MediaLoadHandler::drain_bulk_queue()
{
    const auto is_video_path = [this](const std::string &path) {
        return m_use_video_player_placebo ? VideoPlayerPlacebo::is_video_path(path)
                                          : VideoPlayer::is_video_path(path);
    };
    const auto add_from_path = [this](const std::filesystem::path &path) {
        return m_use_video_player_placebo ? m_video_player_placebo->add_from_path(path)
                                          : m_video_player->add_from_path(path);
    };

    // Cannot upload without a valid Vulkan context.
    if (!m_vk)
        return;

    std::string next_path;

    // try_pop_ready returns false immediately when the queue is empty.
    if (!m_bulk_queue->try_pop_ready(&next_path))
        return;

    // Route the single validated path to the correct consumer.
    const std::string title = std::filesystem::path(next_path).filename().string();

    if (is_video_path(next_path)) {
        // The background worker confirmed the file exists — open in the player.
        if (add_from_path(next_path))
            m_history->push(next_path, "file", title, *m_files_window);
    } else {
        // The background worker confirmed the file exists — upload to the viewer.
        if (m_viewer->add_from_path(next_path, *m_vk))
            m_history->push(next_path, "file", title, *m_files_window);
    }
}

/**
 * @brief Rebuild all video windows from the history list.
 *
 * Called once at startup (and by the "fix" action in VideoContextMenu) to
 * restore the video session from the previous run.  Non-video history entries
 * are skipped — images are opened on demand when the user re-selects them.
 *
 * For URL-sourced videos the downloader cache is consulted first so that
 * previously downloaded files play from disk rather than being streamed again.
 */
void MediaLoadHandler::restore_from_history()
{
    const auto is_video_path = [this](const std::string &path) {
        return m_use_video_player_placebo ? VideoPlayerPlacebo::is_video_path(path)
                                          : VideoPlayer::is_video_path(path);
    };
    const auto is_video_url = [this](const std::string &url) {
        return m_use_video_player_placebo ? VideoPlayerPlacebo::is_video_url(url)
                                          : VideoPlayer::is_video_url(url);
    };
    const auto close_all_windows = [this]() {
        if (m_use_video_player_placebo)
            m_video_player_placebo->close_all_windows();
        else
            m_video_player->close_all_windows();
    };

    // Close any windows that may already be open from a previous call.
    close_all_windows();

    // Track whether any cached_path fields were updated so we can persist once.
    bool history_changed = false;

    for (auto &h : m_history->entries()) {
        // Skip entries not marked for startup restore.
        if (!h.startup_restore)
            continue;

        // Skip non-video entries — images are opened on demand.
        const bool is_video = is_video_path(h.source) || is_video_url(h.source);
        if (!is_video)
            continue;

        // ---- URL-sourced video ------------------------------------------
        if (is_video_url(h.source)) {
            std::filesystem::path cached_path;
            std::error_code ec; // used for non-throwing filesystem queries

            // Check whether the history entry already knows a local cached file.
            if (!h.cached_path.empty()) {
                const std::filesystem::path cp(h.cached_path);
                if (std::filesystem::exists(cp, ec))
                    cached_path = cp; // confirmed: the file still exists on disk
            }

            // If no valid cache path yet, ask the downloader (may start a download).
            if (cached_path.empty()) {
                if (const auto dl = m_downloader->get_or_enqueue(h.source)) {
                    cached_path       = *dl;           // a download just completed
                    h.cached_path     = dl->string();  // store the new path in history
                    history_changed   = true;          // will need to re-persist
                }
            }

            // Play from disk if a cached file is available, otherwise stream.
            if (!cached_path.empty()) {
                const std::string title = h.title.empty()
                    ? ImageDownloader::title_from_url(h.source)
                    : h.title;
                if (m_use_video_player_placebo)
                    m_video_player_placebo->add_from_path(cached_path,
                                                          title,
                                                          h.source,
                                                          h.hwdec_enabled,
                                                          h.resume_position_seconds,
                                                          {});
                else
                    m_video_player->add_from_path(cached_path,
                                                  title,
                                                  h.source,
                                                  h.hwdec_enabled,
                                                  h.resume_position_seconds,
                                                  {}); // local cached file
            } else {
                // Fall back to streaming the URL directly.
                const std::string title = h.title.empty()
                    ? ImageDownloader::title_from_url(h.source)
                    : h.title;
                if (m_use_video_player_placebo)
                    m_video_player_placebo->add_from_url(h.source,
                                                         title,
                                                         h.hwdec_enabled,
                                                         h.resume_position_seconds,
                                                         {});
                else
                    m_video_player->add_from_url(h.source,
                                                 title,
                                                 h.hwdec_enabled,
                                                 h.resume_position_seconds,
                                                 {});
            }
            continue; // move to the next history entry
        }

        // ---- Local-file video -------------------------------------------
        std::error_code ec;
        const std::filesystem::path local_path(h.source);

        // Only open if the file still exists — it may have been deleted since.
        if (std::filesystem::exists(local_path, ec)) {
            if (m_use_video_player_placebo) {
                m_video_player_placebo->add_from_path(local_path,
                                                      h.title.empty() ? local_path.filename().string() : h.title,
                                                      h.source,
                                                      h.hwdec_enabled,
                                                      h.resume_position_seconds,
                                                      {});
            } else {
                m_video_player->add_from_path(local_path,
                                              h.title.empty() ? local_path.filename().string() : h.title,
                                              h.source,
                                              h.hwdec_enabled,
                                              h.resume_position_seconds,
                                              {});
            }
        }
    }

    // If any cached_path fields were newly populated, write the update to disk.
    if (history_changed)
        m_history->persist();
}

void MediaLoadHandler::set_use_video_player_placebo(bool enabled)
{
    m_use_video_player_placebo = enabled;
}