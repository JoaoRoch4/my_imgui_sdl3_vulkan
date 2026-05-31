/**
 * @file history_preview.cpp
 * @brief Hover-thumbnail preview for the history list.
 *
 * draw_for_hover() renders a tooltip-style window next to the cursor when the
 * user hovers over a history entry in the "Recent" menu or the "Opened Files"
 * panel.  The window is kept fully on-screen by clamp_tooltip_pos(), which
 * flips the placement axis-by-axis when the window would otherwise overflow
 * the display edge.
 */

#include "history_preview.hpp"
#include "Image_viewer_panel.hpp"
#include "core/log/debug_log.hpp"
#include "core/thread/thread_overwatch.hpp"
#include "video_hover_preview.hpp"
#include "video_player.hpp"
#include "video_player_placebo.hpp"

#include <curl/curl.h>

#include <string_view>
#include <utility>
#include <unistd.h>

/// Global cap on the image preview size (width × height in pixels).
ImVec2 g_history_preview_max_size = ImVec2(1000.0f, 1000.0f);

// ============================================================================
// File-local helpers
// ============================================================================

namespace {

/// Accumulation buffer used by the libcurl write callback.
struct CurlBuf {
    std::vector<uint8_t> data; ///< Raw bytes received from the network.
};

/// libcurl write callback — appends received bytes into a CurlBuf.
size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *user) { // NOLINT
    auto       *buf   = static_cast<CurlBuf *>(user);
    const auto *bytes = static_cast<const uint8_t *>(ptr);
    buf->data.insert(buf->data.end(), bytes, bytes + size * nmemb); // append chunk
    return size * nmemb; // return bytes consumed; mismatch aborts the transfer
}

/// Derive the image file extension from a URL, defaulting to ".jpg".
std::string ext_from_url(const std::string &url) {
    const std::string clean = url.substr(0, url.find('?')); // strip query string
    std::string ext = std::filesystem::path(clean).extension().string();

    // Only accept extensions the image decoder actually supports.
    constexpr std::array<std::string_view, 7> valid{
        ".jpg", ".jpeg", ".png", ".bmp", ".tga", ".gif", ".webp"};

    for (auto v : valid)
        if (ext == v)
            return ext; // known extension — return as-is

    return ".jpg"; // unknown or missing — fall back to JPEG
}

/// Download a URL into a uniquely-named temp file with the correct extension.
std::filesystem::path download_to_temp(const std::string &url) {
    CurlBuf buf;

    CURL *curl = curl_easy_init(); // create a new easy handle
    if (!curl)
        return {}; // curl not available — abort

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // follow HTTP redirects
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L); // give up after 30 seconds

    const CURLcode res = curl_easy_perform(curl); // blocking download
    curl_easy_cleanup(curl);                      // always release the handle

    if (res != CURLE_OK || buf.data.empty())
        return {}; // download failed or produced no bytes

    // Create a uniquely-named temp file; mkstemp guarantees no name collision.
    char tmp_tpl[] = "/tmp/imgpreview_XXXXXX";
    const int fd = mkstemp(tmp_tpl);
    if (fd < 0)
        return {}; // OS denied the temp file — bail out
    close(fd);    // close the raw fd; we'll write through std::ofstream

    // Append the correct extension so the image decoder can detect the format.
    std::filesystem::path final_path = std::string(tmp_tpl) + ext_from_url(url);

    // Rename the placeholder to the extension-carrying path.
    std::error_code ec;
    std::filesystem::rename(tmp_tpl, final_path, ec);
    if (ec)
        return {}; // rename failed (cross-device?) — give up

    // Write the downloaded bytes into the final file.
    std::ofstream ofs(final_path, std::ios::binary);
    if (!ofs) {
        std::filesystem::remove(final_path); // clean up the empty file
        return {};
    }

    // std::bit_cast to const char* is correct here — buf.data holds raw bytes.
    ofs.write(static_cast<const char *>(static_cast<const void *>(buf.data.data())),
              static_cast<std::streamsize>(buf.data.size()));

    return final_path; // caller is responsible for deleting this file
}

} // namespace

// ============================================================================
// Construction / destruction
// ============================================================================

/**
 * @brief Default-initialise all pointer and flag members.
 */
HistoryPreview::HistoryPreview()
    : m_vk{nullptr}
    , m_video_player{nullptr}
    , m_video_player_placebo{nullptr}
    , m_viewer{nullptr}
    , m_use_video_player_placebo{false}
    , m_worker{}
    , m_worker_watch_id{0}
    , m_mutex{}
    , m_cv{}
    , m_has_pending_job{false}
    , m_pending_job{}
    , m_has_ready_result{false}
    , m_ready_result{}
    , m_active_texture{}
    , m_active_source{}
    , m_active_temp_path{}
    , m_thumb_dir{}
{
}

HistoryPreview::~HistoryPreview()
{
    shutdown(); // release GPU resources and stop the worker thread
}

// ============================================================================
// Public lifecycle
// ============================================================================

/**
 * @brief Store external dependencies and start the background worker thread.
 *
 * Safe to call again after shutdown() — reinitialises all state cleanly.
 *
 * @param vk     Active Vulkan context (must remain valid until shutdown()).
 * @param vp     Optional VideoPlayer used for video thumbnail queries.
 * @param viewer Optional ImageViewerPanel for already-open image look-ups.
 */
void HistoryPreview::setup(vulkan_context *vk,
                           VideoPlayer      *vp,
                           ImageViewerPanel *viewer,
                           VideoPlayerPlacebo *vpp,
                           bool use_video_player_placebo)
{
    shutdown(); // idempotent — resets any previous state first

    m_vk           = vk;
    m_video_player = vp;
    m_video_player_placebo = vpp;
    m_viewer       = viewer;
    m_use_video_player_placebo = use_video_player_placebo;

    // Reset all job / result state so no stale data leaks into the new session.
    m_has_pending_job  = false;
    m_has_ready_result = false;
    m_pending_job      = Job{};
    m_ready_result     = JobResult{};
    m_saved_video_thumbnail_sources.clear();

    start_worker_thread(); // start the background download/existence-check loop
}

/**
 * @brief Stop the worker thread, release the active GPU texture, and clean up
 *        any temporary files produced during this session.
 */
void HistoryPreview::shutdown()
{
    // Signal the worker to stop and drain any in-flight job.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_has_pending_job = false; // discard any queued job before joining
    }
    stop_worker_thread(true); // joins the thread

    clear_active_preview(); // frees GPU texture and removes the temp file

    // The ready result may also carry a temp file that was never consumed.
    if (m_has_ready_result && m_ready_result.is_temp_file && !m_ready_result.path.empty()) {
        std::error_code ec;
        std::filesystem::remove(m_ready_result.path, ec); // ignore error — best effort
    }

    // Reset result state so a subsequent setup() starts cleanly.
    m_has_ready_result = false;
    m_ready_result     = JobResult{};
    m_saved_video_thumbnail_sources.clear();
    m_vk               = nullptr;
}

// ============================================================================
// Worker thread management
// ============================================================================

/**
 * @brief Start the background worker thread and register it with ThreadOverwatch.
 *
 * Safe to call when the thread is already running (returns immediately).
 */
void HistoryPreview::start_worker_thread()
{
    if (m_worker.joinable())
        return; // already running — nothing to do

    m_worker = std::jthread{[this](std::stop_token st) { worker_loop(std::move(st)); }};

    // Register with the watchdog only on the very first start (id == 0).
    const uint64_t current_watch = m_worker_watch_id.load(std::memory_order_acquire);
    if (current_watch == 0) {
        const auto watch_id = ThreadOverwatch::instance().watch(
            "HistoryPreview::worker",
            std::chrono::milliseconds(5000),
            [this]() { stop_worker_thread(false); },   // on timeout: stop without unregistering
            [this]() {                                  // on recovery: restart
                if (m_vk != nullptr)
                    start_worker_thread();
            });
        m_worker_watch_id.store(watch_id, std::memory_order_release);
    }

    // Kick the watchdog immediately so it doesn't fire before the first real heartbeat.
    ThreadOverwatch::instance().heartbeat(
        m_worker_watch_id.load(std::memory_order_acquire));
}

/**
 * @brief Request the worker to stop and optionally unregister it from ThreadOverwatch.
 *
 * @param unregister_watch  True during normal shutdown; false when called from
 *                          the watchdog's own timeout handler.
 */
void HistoryPreview::stop_worker_thread(bool unregister_watch)
{
    if (unregister_watch) {
        // Exchange to 0 — the returned id is the one we registered.
        const uint64_t watch_id = m_worker_watch_id.exchange(0, std::memory_order_acq_rel);
        ThreadOverwatch::instance().unwatch(watch_id);
    }

    if (m_worker.joinable()) {
        m_worker.request_stop(); // cooperative cancellation via stop_token
        m_cv.notify_all();       // wake the worker if it is blocked on wait_for
        m_worker.join();         // block until the thread exits
    }
}

// ============================================================================
// Preview request / result cycle
// ============================================================================

/**
 * @brief Queue a background job to resolve the preview image for hentry.
 *
 * If a job for the same source is already pending, the call is a no-op.
 *
 * @param hentry  History entry whose source and kind are used for the job.
 */
void HistoryPreview::request_preview(const WindowStateToml::ImageHistoryEntry &hentry)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Skip duplicate requests so the worker does not churn on the same source.
    if (m_has_pending_job &&
        m_pending_job.source == hentry.source &&
        m_pending_job.kind   == hentry.kind)
        return;

    APP_DEBUG_LOG("[HistoryPreview] request_preview: kind={} source={}",
                  hentry.kind, hentry.source);

    m_pending_job      = Job{hentry.source, hentry.kind};
    m_has_pending_job  = true;
    m_cv.notify_one(); // wake the worker
}

/**
 * @brief Release the currently active GPU texture and remove its temp file.
 */
void HistoryPreview::clear_active_preview()
{
    if (m_vk && m_active_texture.is_loaded()) {
        vkDeviceWaitIdle(m_vk->device); // wait for GPU before freeing resources
        m_active_texture.unload(*m_vk);
    }

    if (!m_active_temp_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(m_active_temp_path, ec); // best-effort cleanup
        m_active_temp_path.clear();
    }

    m_active_source.clear();
}

/**
 * @brief Upload a completed worker result to the GPU if it matches hentry.
 *
 * Takes ownership of the JobResult under the lock, releases the lock, then
 * loads the texture on the main thread where Vulkan calls are safe.
 *
 * @param hentry  The history entry currently being rendered in the tooltip.
 */
void HistoryPreview::apply_ready_result(const WindowStateToml::ImageHistoryEntry &hentry)
{
    JobResult result;
    bool      has_result = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Only consume the result if it matches what is currently being hovered.
        if (m_has_ready_result && m_ready_result.source == hentry.source) {
            result           = std::move(m_ready_result);
            m_ready_result   = JobResult{};
            m_has_ready_result = false;
            has_result       = true;
        }
    }

    if (!has_result)
        return; // nothing ready for this source — keep waiting

    clear_active_preview(); // free the previous texture before loading a new one

    if (result.ok && m_vk && m_active_texture.load(result.path, *m_vk)) {
        m_active_source = result.source; // mark the texture as loaded for this source
        if (result.is_temp_file)
            m_active_temp_path = result.path; // remember to delete when the preview changes
    } else if (result.is_temp_file && !result.path.empty()) {
        // Load failed — still need to clean up the temp file.
        std::error_code ec;
        std::filesystem::remove(result.path, ec);
    }
}

// ============================================================================
// Thumbnail directory
// ============================================================================

/**
 * @brief Set the directory where video thumbnail PNGs are written and cached.
 *
 * Creates the directory if it does not exist.  A no-op when dir is empty.
 *
 * @param dir  Absolute path to the thumbnail cache directory.
 */
void HistoryPreview::set_thumb_dir(const std::filesystem::path &dir)
{
    m_thumb_dir = dir;
    if (m_thumb_dir.empty())
        return; // no dir configured — thumbnail saving is disabled

    std::error_code ec;
    std::filesystem::create_directories(m_thumb_dir, ec);
    if (ec) {
        APP_DEBUG_LOG("[HistoryPreview] failed to create thumb dir: {} ({})",
                      m_thumb_dir.string(), ec.message());
    }
}

// ============================================================================
// Tooltip positioning
// ============================================================================

/**
 * @brief Compute a tooltip window position that stays fully within the display.
 *
 * Strategy per axis:
 *   - Try to place the window edge `offset` pixels to the right of / below
 *     the cursor hot-spot.
 *   - If that would push the far edge past the display boundary, flip to the
 *     opposite side (left of / above the cursor).
 *   - Clamp the result to [0, display_size - window_size] to handle the corner
 *     case where the window is larger than the space on both sides.
 *
 * The function accounts for ImGui's own window padding so the content area
 * does not clip either.
 *
 * @param mouse        Current mouse position in display/screen coordinates.
 * @param preview_size Expected size of the preview content (image or video frame).
 * @param offset       Gap in pixels between the cursor hot-spot and the near edge
 *                     of the tooltip window.  Default 24 px matches the old value.
 * @return             Screen-space position to pass to ImGui::SetNextWindowPos().
 */
ImVec2 HistoryPreview::clamp_tooltip_pos(ImVec2 mouse,
                                          ImVec2 preview_size,
                                          float  offset)
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;  // full screen resolution
    const ImVec2 pad     = ImGui::GetStyle().WindowPadding; // ImGui inner padding

    // Two text lines (title + timestamp) contribute to the tooltip height.
    const float line_h = ImGui::GetTextLineHeightWithSpacing();

    // Estimate the total window size including padding on every side.
    const ImVec2 win_size = {
        preview_size.x + pad.x * 2.0f,                    // content width + left/right pad
        preview_size.y + line_h * 2.0f + pad.y * 2.0f     // content height + 2 text lines + top/bottom pad
    };

    // ---- Horizontal axis ---------------------------------------------------

    float x = mouse.x + offset; // first attempt: right of cursor

    if (x + win_size.x > display.x)           // would overflow the right edge?
        x = mouse.x - win_size.x - offset;    // flip: place to the left instead

    // Final clamp: never let the window start off the left or right boundary.
    x = std::clamp(x, 0.0f, std::max(0.0f, display.x - win_size.x));

    // ---- Vertical axis -----------------------------------------------------

    float y = mouse.y + offset; // first attempt: below cursor

    if (y + win_size.y > display.y)           // would overflow the bottom edge?
        y = mouse.y - win_size.y - offset;    // flip: place above the cursor instead

    // Final clamp: never let the window start above the top or below the bottom.
    y = std::clamp(y, 0.0f, std::max(0.0f, display.y - win_size.y));

    return {x, y};
}

// ============================================================================
// Main hover-draw entry point
// ============================================================================

/**
 * @brief Render the preview tooltip for a history entry being hovered.
 *
 * The tooltip window is placed next to the cursor and clamped so it never
 * extends past any edge of the display.  The expected content size is used
 * to select a placement quadrant before the window is actually opened, so
 * the clamp is based on a conservative size estimate rather than the actual
 * rendered size (which ImGui does not expose before the window is measured).
 *
 * Content rendered per entry kind:
 *   - Video (file or URL) — live thumbnail from VideoHoverPreview; falls back
 *     to a saved PNG if the live thumbnail is not yet ready.
 *   - Image URL           — uses the already-open ImGui texture if the viewer
 *     has it; otherwise loads via the background worker.
 *   - Image file          — loaded via the background worker on first hover.
 *
 * @param hentry  The history entry currently under the cursor.  thumbnail_path
 *                may be written on this entry when a new video thumbnail is saved.
 */
void HistoryPreview::draw_for_hover(WindowStateToml::ImageHistoryEntry &hentry)
{
    const auto has_video_backend = [this]() {
        return m_use_video_player_placebo ? m_video_player_placebo != nullptr
                                          : m_video_player != nullptr;
    };
    const auto is_video_path = [this](const std::filesystem::path &path) {
        return m_use_video_player_placebo ? VideoPlayerPlacebo::is_video_path(path)
                                          : VideoPlayer::is_video_path(path);
    };
    const auto is_video_url = [this](const std::string &url) {
        return m_use_video_player_placebo ? VideoPlayerPlacebo::is_video_url(url)
                                          : VideoPlayer::is_video_url(url);
    };

    if (!m_vk)
        return; // Vulkan not ready — cannot upload or render any texture

    // Decide whether this entry is a video so we can choose the right thumbnail path.
    const bool is_video = has_video_backend() &&
                          (hentry.kind == "file"
                               ? is_video_path(std::filesystem::path(hentry.source))
                               : is_video_url(hentry.source));

    // ------------------------------------------------------------------
    // Compute the clamped tooltip position before opening the window.
    //
    // We use the maximum possible content size for the placement decision:
    //   - Videos are always preview_size (fixed 320×180).
    //   - Images are bounded by g_history_preview_max_size but may be smaller
    //     after aspect-ratio scaling.  Over-estimating shifts the flip point
    //     slightly early, which is harmless — the window always stays on screen.
    // ------------------------------------------------------------------
    const ImVec2 expected_content = is_video
        ? VideoHoverPreview::preview_size   // fixed frame size
        : g_history_preview_max_size;       // conservative upper bound for images

    const ImVec2 mouse      = ImGui::GetMousePos();
    const ImVec2 tooltip_pos = clamp_tooltip_pos(mouse, expected_content);

    if (is_video && has_video_backend()) {
        // If hover preview is disabled globally, skip the tooltip entirely.
        if (!VideoHoverPreview::enabled)
            return;

        // Determine the actual source we will look up (cached path or original).
        const bool has_cached = !hentry.cached_path.empty() &&
            std::filesystem::exists(std::filesystem::path(hentry.cached_path));
        const std::string &lookup_src = has_cached ? hentry.cached_path : hentry.source;

        // Tick the dwell timer so it counts down while no tooltip is shown.
        if (m_use_video_player_placebo)
            m_video_player_placebo->notify_hover(lookup_src);
        else
            m_video_player->notify_hover(lookup_src);

        // Suppress the tooltip entirely while the hover dwell has not elapsed.
        [[maybe_unused]] bool _ = m_use_video_player_placebo
            ? m_video_player_placebo->consume_hover_popup_reopen_request()
            : m_video_player->consume_hover_popup_reopen_request(); // drain the pulse
        if (m_use_video_player_placebo
                ? m_video_player_placebo->is_hover_dwell_pending(lookup_src)
                : m_video_player->is_hover_dwell_pending(lookup_src))
            return; // tooltip stays closed during the dwell window
    }

    // Apply the clamped position — must be called before BeginTooltip().
    ImGui::SetNextWindowPos(tooltip_pos, ImGuiCond_Always);
    ImGui::BeginTooltip();

    // Title line — prefer the stored title, fall back to the filename component.
    const std::string &display_title = !hentry.title.empty()
        ? hentry.title
        : std::filesystem::path(hentry.source).filename().string();
    ImGui::TextUnformatted(display_title.c_str());

    // ------------------------------------------------------------------
    // Content — diverges for video vs image
    // ------------------------------------------------------------------

    if (is_video) {
        if (!hentry.thumbnail_path.empty())
            m_saved_video_thumbnail_sources.insert(hentry.source);

        // Prefer the cached local file when one is available; it avoids a
        // network round-trip and is more reliable for seeking.
        const bool has_cached_file =
            !hentry.cached_path.empty() &&
            std::filesystem::exists(std::filesystem::path(hentry.cached_path));
        const std::string &lookup_src = has_cached_file ? hentry.cached_path : hentry.source;

        // Ask the player for a live thumbnail.  get_open_thumbnail() returns the
        // descriptor of a window that is already playing; hover_thumbnail() uses
        // VideoHoverPreview for a lightweight seek-based preview.
        VkDescriptorSet ds = m_use_video_player_placebo
            ? m_video_player_placebo->get_open_thumbnail(lookup_src)
            : m_video_player->get_open_thumbnail(lookup_src);
        if (ds == VK_NULL_HANDLE)
            ds = m_use_video_player_placebo
                ? m_video_player_placebo->hover_thumbnail(lookup_src)
                : m_video_player->hover_thumbnail(lookup_src);

        if (ds != VK_NULL_HANDLE) {
            // Live frame available — render it, correcting for aspect ratio.
            // mpv SW renderer fills the entire preview_size buffer by stretching;
            // compute the display size from the source's native dimensions so the
            // content appears with correct proportions.
            const ImVec2 src_dims = VideoHoverPreview::last_source_size;
            ImVec2 draw_size = VideoHoverPreview::preview_size;
            if (src_dims.x > 0.0f && src_dims.y > 0.0f) {
                const float scale = std::min(draw_size.x / src_dims.x, draw_size.y / src_dims.y);
                draw_size = ImVec2(src_dims.x * scale, src_dims.y * scale);
            }
            ImGui::Image(std::bit_cast<ImTextureID>(ds), draw_size);

            // Opportunistically save this frame as a static thumbnail PNG so
            // that subsequent hovers can show something while the live preview loads.
            const bool already_saved =
                m_saved_video_thumbnail_sources.contains(hentry.source);
            if (!already_saved && hentry.thumbnail_path.empty() && !m_thumb_dir.empty()) {
                // FNV-1a hash of the source string used as the filename.
                const auto fnv = [](const std::string &s) {
                    uint64_t h = 14695981039346656037ULL;
                    for (const unsigned char c : s) {
                        h ^= c;
                        h *= 1099511628211ULL;
                    }
                    return h;
                };

                char hex[17];
                std::snprintf(hex, sizeof(hex), "%016llx",
                              static_cast<unsigned long long>(fnv(hentry.source)));

                const auto tp = m_thumb_dir / (std::string(hex) + ".png"); // unique path
                std::error_code ec;
                std::filesystem::create_directories(tp.parent_path(), ec);
                if (ec) {
                    APP_DEBUG_LOG("[HistoryPreview] failed to create thumb dir: {} ({})",
                                  tp.parent_path().string(), ec.message());
                }

                APP_DEBUG_LOG("[HistoryPreview] trying save_hover_frame: {}", tp.string());
                if (!ec && (m_use_video_player_placebo
                                ? m_video_player_placebo->save_hover_frame(tp)
                                : m_video_player->save_hover_frame(tp))) {
                    hentry.thumbnail_path = tp.string(); // persist the path in history
                    m_saved_video_thumbnail_sources.insert(hentry.source);
                    APP_DEBUG_LOG("[HistoryPreview] thumbnail saved: {}", tp.string());
                }
            }
        } else {
            // No live frame yet — try the cached static thumbnail PNG.
            bool drew_static_thumb = false;

            if (!hentry.thumbnail_path.empty()) {
                const std::filesystem::path tp(hentry.thumbnail_path);
                if (std::filesystem::exists(tp)) {
                    // Load the PNG into a Vulkan texture if not already loaded.
                    if (m_active_source != hentry.source) {
                        clear_active_preview(); // unload any previously active texture
                        if (m_active_texture.load(tp, *m_vk))
                            m_active_source = hentry.source;
                    }

                    if (m_active_texture.is_loaded()) {
                        // Scale the PNG to fit within preview_size, preserving aspect ratio.
                        const float src_w = static_cast<float>(m_active_texture.width);
                        const float src_h = static_cast<float>(m_active_texture.height);
                        const float max_w = VideoHoverPreview::preview_size.x;
                        const float max_h = VideoHoverPreview::preview_size.y;
                        const float scale = std::min(max_w / src_w, max_h / src_h);
                        ImGui::Image(m_active_texture.imgui_id(),
                                     ImVec2(src_w * scale, src_h * scale));
                        drew_static_thumb = true;
                    }
                } else {
                    hentry.thumbnail_path.clear(); // stale path — reset so it regenerates
                    m_saved_video_thumbnail_sources.erase(hentry.source);
                }
            }

            if (!drew_static_thumb)
                ImGui::TextDisabled("Loading preview..."); // placeholder while mpv seeks
        }

    } else {
        // ---- Image entry ---------------------------------------------------

        // Fast path: the image is already open in the viewer panel — reuse its texture.
        if (hentry.kind == "url" && m_viewer) {
            const ImTextureID existing = m_viewer->get_imgui_id_for_source(hentry.source);
            if (existing) {
                ImGui::Image(existing, g_history_preview_max_size);
                ImGui::TextDisabled("%s", hentry.opened_at.c_str());
                ImGui::EndTooltip();
                return; // early return — timestamp already drawn above
            }
        }

        // Slow path: request a background load and apply the result once ready.
        if (m_active_source != hentry.source)
            request_preview(hentry); // enqueue download or existence-check

        apply_ready_result(hentry); // upload to GPU if the worker finished

        if (m_active_source == hentry.source && m_active_texture.is_loaded()) {
            // Scale to fit g_history_preview_max_size, but never upscale beyond 1:1.
            const auto src_w  = static_cast<float>(m_active_texture.width);
            const auto src_h  = static_cast<float>(m_active_texture.height);
            const float max_w  = g_history_preview_max_size.x;
            const float max_h  = g_history_preview_max_size.y;
            const float scale  = std::min(max_w / src_w, max_h / src_h);
            const float draw_w = src_w * std::min(scale, 1.0f); // never upscale
            const float draw_h = src_h * std::min(scale, 1.0f);
            ImGui::Image(m_active_texture.imgui_id(), ImVec2(draw_w, draw_h));
        } else {
            ImGui::TextDisabled("Loading preview..."); // placeholder while worker runs
        }
    }

    // Timestamp footer shown for all entry types.
    ImGui::TextDisabled("%s", hentry.opened_at.c_str());
    ImGui::EndTooltip();
}

void HistoryPreview::set_use_video_player_placebo(bool enabled)
{
    m_use_video_player_placebo = enabled;
}

// ============================================================================
// Background worker loop
// ============================================================================

/**
 * @brief Thread body — waits for jobs, resolves them, and posts results.
 *
 * For "file" jobs: checks whether the path exists (cheap — no I/O needed).
 * For "url"  jobs: downloads the image to a temp file via libcurl (blocking).
 *
 * Results are posted under the mutex so draw_for_hover() can consume them
 * safely on the main thread via apply_ready_result().
 *
 * @param stoken  Cooperative stop token; the loop exits when stop is requested.
 */
void HistoryPreview::worker_loop(std::stop_token stoken)
{
    for (;;) {
        const uint64_t watch_id = m_worker_watch_id.load(std::memory_order_acquire);
        ThreadOverwatch::instance().heartbeat(watch_id); // keep the watchdog happy

        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);

            // Block until a job arrives or the thread is asked to stop.
            m_cv.wait_for(lock, std::chrono::milliseconds(500), [this, &stoken] {
                return stoken.stop_requested() || m_has_pending_job;
            });

            ThreadOverwatch::instance().heartbeat(watch_id); // heartbeat after wake

            if (stoken.stop_requested())
                break; // cooperative exit

            if (!m_has_pending_job)
                continue; // spurious wake — go back to waiting

            // Take ownership of the pending job so we can release the lock.
            job                = std::move(m_pending_job);
            m_pending_job      = Job{};
            m_has_pending_job  = false;
        }

        // -- Resolve the job (outside the lock so the main thread is not blocked) --

        JobResult result;
        result.source       = job.source;
        result.is_temp_file = false;
        result.ok           = false;

        if (job.kind == "file") {
            // Local file: just verify it still exists — no data read needed.
            const std::filesystem::path path(job.source);
            if (std::filesystem::exists(path)) {
                result.path = path;
                result.ok   = true;
            }
        } else if (job.kind == "url") {
            // URL: blocking download to a temp file.
            result.path        = download_to_temp(job.source);
            result.is_temp_file = !result.path.empty(); // temp file must be deleted by consumer
            result.ok          = result.is_temp_file;
        }

        // -- Post the result under the lock --

        std::lock_guard<std::mutex> lock(m_mutex);

        if (stoken.stop_requested()) {
            // Main thread is shutting down — clean up the temp file ourselves.
            if (result.is_temp_file && !result.path.empty()) {
                std::error_code ec;
                std::filesystem::remove(result.path, ec);
            }
            break;
        }

        // Discard any previous unconsumed result that is about to be replaced.
        if (m_has_ready_result &&
            m_ready_result.is_temp_file &&
            !m_ready_result.path.empty())
        {
            std::error_code ec;
            std::filesystem::remove(m_ready_result.path, ec); // avoid temp-file leak
        }

        m_ready_result     = std::move(result);
        m_has_ready_result = true; // signal to draw_for_hover() that data is ready

        ThreadOverwatch::instance().heartbeat(watch_id);
    }
}