/**
 * @file main_menu_bar.cpp
 * @brief Implementation of MainMenuBar.
 *
 * This file handles:
 *   - File dialog and URL dialog management.
 *   - Decoding / downloading images and handing them to ImageViewerPanel.
 *   - Rendering the ImGui menu bar and the "Open Online" popup.
 *   - History persistence (up to 100 entries, 20 shown at once).
 *
 * Image rendering (zoom, pan, per-window draw) is fully delegated to
 * ImageViewerPanel::draw_windows() — see image_viewer_panel.cpp.
 */

#include "main_menu_bar.hpp"
#include "Image_viewer_panel.hpp"

#include "imgui.h"
#include "style_editor.hpp"

#include <curl/curl.h>
#include <SDL3/SDL_dialog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

// ============================================================================
// Anonymous-namespace helpers (file-local, not part of any class)
// ============================================================================

namespace {

/// Buffer accumulator used by the libcurl write callback.
struct CurlBuf {
    std::vector<uint8_t> data;
};

/**
 * @brief libcurl write callback — appends downloaded bytes to a CurlBuf.
 *
 * @param ptr    Pointer to the data block delivered by curl.
 * @param size   Always 1 (curl convention).
 * @param nmemb  Number of bytes in the block.
 * @param user   Pointer to our CurlBuf, passed via CURLOPT_WRITEDATA.
 * @return       Number of bytes handled (must equal size * nmemb on success).
 */
static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *user) {
    auto *buf = static_cast<CurlBuf *>(user);                       /// Cast the user pointer to our buffer.
    const auto *bytes = static_cast<const uint8_t *>(ptr);          /// Cast the data pointer to bytes.
    buf->data.insert(buf->data.end(), bytes, bytes + size * nmemb); /// Append to accumulator.
    return size * nmemb;                                            /// Tell curl we consumed everything.
}

/**
 * @brief Derive a file extension from a URL path component.
 *
 * Strips the query string first (everything from '?' onward), then
 * extracts the extension with std::filesystem::path.  Falls back to
 * ".jpg" if the extension is not in the recognised list.
 *
 * @param url  Full URL string (may contain a query string).
 * @return     Extension string including the leading dot, e.g. ".png".
 */
static std::string ext_from_url(const std::string &url) {
    /// Remove the query string (e.g. "?size=large") before path parsing.
    const std::string clean = url.substr(0, url.find('?'));

    /// Extract the extension from the path component of the (cleaned) URL.
    std::string ext = std::filesystem::path(clean).extension().string();

    /// Allowlist of extensions the texture loader can handle.
    constexpr std::array<std::string_view, 7> valid{
        ".jpg", ".jpeg", ".png", ".bmp", ".tga", ".gif", ".webp"};

    /// Return the extension only if it is in the allowlist.
    for (auto v : valid)
        if (ext == v)
            return ext;

    return ".jpg"; /// Default fallback.
}

/**
 * @brief Download a URL into a uniquely named temp file.
 *
 * The file is named with a random suffix via mkstemp() and the correct
 * image extension appended so the decoder can detect the format.
 *
 * @param url  URL to download.
 * @return     Path to the temp file on success, or an empty path on failure.
 *             The CALLER is responsible for deleting the file.
 */
static std::filesystem::path download_to_temp(const std::string &url) {
    CurlBuf buf;

    /// Initialise a libcurl easy handle.
    CURL *curl = curl_easy_init();
    if (!curl)
        return {}; /// curl failed to initialise.

    /// Configure the transfer.
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());             /// Target URL.
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb); /// Our write callback.
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);              /// Accumulator argument.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);           /// Follow HTTP redirects.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);                 /// 30-second hard timeout.

    const CURLcode res = curl_easy_perform(curl); /// Execute the download.
    curl_easy_cleanup(curl);                      /// Always free the handle.

    if (res != CURLE_OK || buf.data.empty())
        return {}; /// Download failed or produced no data.

    /**
     * Create a temp file with a random name.
     * mkstemp() opens the file and returns an fd; we close it immediately
     * and rename to add the correct extension.
     */
    char tmp_tpl[] = "/tmp/imgview_XXXXXX";
    const int fd = mkstemp(tmp_tpl);
    if (fd < 0)
        return {}; /// mkstemp failed (e.g. /tmp is full or read-only).
    close(fd);     /// We only needed the unique name, not the open fd.

    /// Append the image extension to the random name.
    std::filesystem::path final_path = std::string(tmp_tpl) + ext_from_url(url);

    /// Rename the empty placeholder to the final path.
    std::error_code ec;
    std::filesystem::rename(tmp_tpl, final_path, ec);
    if (ec)
        return {}; /// Rename failed.

    /// Write the downloaded bytes into the final path.
    std::ofstream ofs(final_path, std::ios::binary);
    if (!ofs) {
        std::filesystem::remove(final_path); /// Clean up on failure.
        return {};
    }
    ofs.write(reinterpret_cast<const char *>(buf.data.data()), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
              static_cast<std::streamsize>(buf.data.size()));

    return final_path;
}

/**
 * @brief Extract a human-readable title from a URL (the filename part).
 *
 * @param url  Full URL string.
 * @return     Filename component of the URL path (e.g. "cat.jpg"),
 *             or "online_image" as a fallback.
 */
static std::string title_from_url(const std::string &url) {
    const std::string clean = url.substr(0, url.find('?')); /// Strip query string.
    auto t = std::filesystem::path(clean).filename().string();
    return t.empty() ? "online_image" : t;
}

} // namespace

// ============================================================================
// Construction
// ============================================================================

/**
 * @brief Default constructor — zero-initialises all pointer members.
 */
MainMenuBar::MainMenuBar()
    : request_quit{false}
    , m_style_editor{nullptr}
    , m_window{nullptr}
    , m_vk{nullptr}
    , m_show_demo_window{nullptr}
    , m_show_another_window{nullptr}
    , m_viewer{} /// Default-construct the image panel.
    , m_history{}
    , m_has_pending_path{false}
    , m_pending_paths{}
    , m_pending_urls{}
    , m_show_url_popup{false}
    , m_url_buf{} {
}

// ============================================================================
// Public lifecycle
// ============================================================================

/**
 * @brief Store external dependencies and initialise libcurl.
 *
 * @param style_editor        Pointer to the style-editor window (may be null).
 * @param window              The SDL3 parent window (for file dialogs).
 * @param vk                  Active Vulkan context.
 * @param show_demo_window    Pointer to the ImGui demo-window toggle.
 * @param show_another_window Pointer to the "Another Window" toggle.
 */
void MainMenuBar::Setup(StyleEditor *style_editor,
                        SDL_Window *window,
                        vulkan_context *vk,
                        bool *show_demo_window,
                        bool *show_another_window) {
    m_style_editor = style_editor;
    m_window = window;
    m_vk = vk;
    m_show_demo_window = show_demo_window;
    m_show_another_window = show_another_window;

    /// Initialise the libcurl global state once per process.
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

/**
 * @brief Shut down the image panel and libcurl.
 *
 * Must be called before ImGui_ImplVulkan_Shutdown().
 */
void MainMenuBar::Shutdown() {
    if (!m_vk)
        return;

    /// Let ImageViewerPanel free all VkImage / VkSampler / etc. resources.
    m_viewer.shutdown(*m_vk);

    /// Release libcurl global state (matches the curl_global_init in Setup).
    curl_global_cleanup();
}

// ============================================================================
// History persistence
// ============================================================================

/**
 * @brief Restore image history from the persisted window state.
 * @param state  Loaded TOML state object.
 */
void MainMenuBar::ApplyHistory(const WindowStateToml &state) {
    m_history = state.image_history; /// Replace in-memory history with the saved list.
}

/**
 * @brief Serialise image history into the window state for saving.
 *
 * Caps at 100 entries to keep the TOML file manageable.
 *
 * @param state  Output TOML state object.
 */
void MainMenuBar::ExportHistory(WindowStateToml *state) const {
    constexpr size_t k_max = 100; /// Maximum history entries written to disk.

    const size_t count = std::min(k_max, m_history.size()); /// Don't exceed the vector size.

    state->image_history = std::vector<WindowStateToml::ImageHistoryEntry>(
        m_history.begin(),
        m_history.begin() + static_cast<std::ptrdiff_t>(count));
}

// ============================================================================
// Private helpers
// ============================================================================

/**
 * @brief SDL3 file-dialog callback — called on the main thread when the user
 *        confirms a file selection.
 *
 * SDL guarantees this is called on the main thread, which is the same
 * thread that runs Build(), so no synchronisation is needed.
 *
 * @param userdata   Pointer to the owning MainMenuBar instance.
 * @param filelist   NULL-terminated array of selected paths, or nullptr on cancel.
 * @param filter     Index of the active file-filter (unused here).
 */
void MainMenuBar::file_dialog_callback(void *userdata,
                                       const char *const *filelist,
                                       int /*filter*/) {
    auto *self = static_cast<MainMenuBar *>(userdata); /// Recover 'this' from the void*.

    if (filelist == nullptr)
        return; /// User cancelled the dialog.

    /// Append each selected path to the pending list.
    for (int i = 0; filelist[i] != nullptr; ++i) {
        self->m_pending_paths.emplace_back(filelist[i]);
        self->m_has_pending_path = true; /// Signal Build() to process the list.
    }
}

/**
 * @brief Write the current local time as "YYYY-MM-DDTHH:MM:SS" into dst.
 * @param dst  Caller-supplied 20-byte array (including the null terminator).
 */
void MainMenuBar::current_timestamp(std::array<char, 20> &dst) {
    /// Capture the current wall-clock time.
    const auto now_c = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());

    /// Break the time_t into calendar fields using the thread-safe reentrant variant.
    struct tm tm_val{};
    localtime_r(&now_c, &tm_val);

    /// Format into the fixed-size buffer.
    std::strftime(dst.data(), dst.size(), "%Y-%m-%dT%H:%M:%S", &tm_val);
}

/**
 * @brief Insert one entry at the front of the history list.
 *
 * @param source  File path or URL string.
 * @param kind    "file" or "url".
 */
void MainMenuBar::push_history(const std::string &source, const std::string &kind) {
    std::array<char, 20> ts{};
    current_timestamp(ts); /// Generate the timestamp string.

    /// Remove any existing entry with the same source so there are no duplicates.
    std::erase_if(m_history, [&source](const WindowStateToml::ImageHistoryEntry& e) {
        return e.source == source;
    });

    /// Insert at the front so the most recent item appears first.
    m_history.insert(m_history.begin(),
                     WindowStateToml::ImageHistoryEntry{source, kind, ts.data()});
}
void ImageViewerPanel::evict_closed(vulkan_context &vk) {
    // First, identify if we actually have anything to delete
    auto it = std::find_if(m_images.begin(), m_images.end(),
                           [](const ImageEntry &e) { return !e.open; });

    if (it == m_images.end())
        return;

    // Safety: Wait for the RTX 3060 to finish pending frames
    vkDeviceWaitIdle(vk.device);

    for (auto &entry : m_images) {
        if (!entry.open && entry.texture.is_loaded()) {
            // Unload must call ImGui_ImplVulkan_RemoveTexture and vkDestroyImage
            entry.texture.unload(vk);
        }
    }

    // Clean up the vector
    std::erase_if(m_images, [](const ImageEntry &e) { return !e.open; });
}
// ============================================================================
// Per-frame build
// ============================================================================

/**
 * @brief Render the main menu bar and coordinate image loading each frame.
 *
 * Call order inside one ImGui frame:
 *   1. evict_closed()          — free GPU resources for closed images.
 *   2. Load pending file paths — hand them to ImageViewerPanel.
 *   3. Download pending URLs   — hand them to ImageViewerPanel.
 *   4. Draw the menu bar       — File, View menus and sub-menus.
 *   5. Draw the URL popup      — modal dialog for "Open Online...".
 *   6. draw_windows()          — delegate to ImageViewerPanel.
 */
void MainMenuBar::Build() {
    // -------------------------------------------------------------------------
    // Step 1 — evict closed images
    // -------------------------------------------------------------------------

    /**
     * Remove images whose open flag was cleared by the ImGui [x] button.
     * This runs FIRST, before any loading, so that closed slots free up
     * capacity for new images in the same frame.
     *
     * The frame fence was already waited on by the Vulkan backend before
     * NewFrame(), so the GPU is no longer reading last frame's resources.
     */
    if (m_vk)
        m_viewer.evict_closed(*m_vk);

    // -------------------------------------------------------------------------
    // Step 2 — load images from the file dialog queue
    // -------------------------------------------------------------------------

    /**
     * file_dialog_callback() populates m_pending_paths on the main thread.
     * We drain the queue here — at most one full file-dialog batch per frame.
     */
    if (m_has_pending_path && m_vk) {
        m_has_pending_path = false; /// Acknowledge the pending flag immediately.

        for (const auto &path : m_pending_paths) {
            /**
             * Stop adding when the cap is reached so we don't attempt a
             * load that will certainly fail.
             */
            if (m_viewer.is_at_capacity())
                break;

            /**
             * Hand the path to ImageViewerPanel; it decodes, uploads, and
             * registers the window.  If it succeeds, record the history entry.
             */
            if (m_viewer.add_from_path(path, *m_vk))
                push_history(path, "file");
        }

        m_pending_paths.clear(); /// Drain the queue regardless of success.
    }

    // -------------------------------------------------------------------------
    // Step 3 — download and load images from the URL queue
    // -------------------------------------------------------------------------

    if (!m_pending_urls.empty() && m_vk) {
        for (const auto &url : m_pending_urls) {
            /// Skip if the cap was already reached by a previous URL in this batch.
            if (m_viewer.is_at_capacity())
                break;

            /// Blocking download — writes bytes to a temp file with the right extension.
            const std::filesystem::path tmp = download_to_temp(url);

            if (!tmp.empty()) {
                /**
                 * Derive the display title from the URL's filename component
                 * (e.g. "https://example.com/cat.jpg?q=1" → "cat.jpg").
                 */
                const std::string title = title_from_url(url);

                /**
                 * Hand the temp file to ImageViewerPanel.
                 * ImageViewerPanel reads the file; we delete it afterward.
                 */
                if (m_viewer.add_from_url_temp(tmp, title, *m_vk))
                    push_history(url, "url");

                std::filesystem::remove(tmp); /// Clean up the temp file.
            }
        }

        m_pending_urls.clear(); /// Drain the queue.
    }

    // -------------------------------------------------------------------------
    // Step 4 — menu bar
    // -------------------------------------------------------------------------

    if (!ImGui::BeginMainMenuBar())
        return; /// Menu bar not visible (e.g. fullscreen game mode).

    // --- Capacity warning (right-aligned in the menu bar) --------------------

    /**
     * When all slots are full, display a dim warning label on the far right
     * of the menu bar so the user knows why "Open Image" is disabled.
     */
    if (m_viewer.is_at_capacity()) {
        /// Build the warning string using the actual cap constant.
        const std::string warn = "Image limit reached (" + std::to_string(m_viewer.count()) + "/" + std::to_string(8) /// Must match k_max_images in panel.
                                 + ")";

        const float warn_w = ImGui::CalcTextSize(warn.c_str()).x; /// Width of the warning text.
        const float avail = ImGui::GetContentRegionAvail().x;     /// Remaining menu bar width.

        /// Only show the warning if it fits in the available space.
        if (avail > warn_w + 8.0f) {
            /// Shift the cursor right so the text appears at the far-right edge.
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - warn_w - 4.0f);
            ImGui::TextDisabled("%s", warn.c_str());
        }
    }

    // --- File menu -----------------------------------------------------------

    if (ImGui::BeginMenu("File")) {
        /**
         * "Open Image..." is disabled when the viewer is at capacity.
         * BeginDisabled() greys out and blocks interaction with child widgets.
         */
        ImGui::BeginDisabled(m_viewer.is_at_capacity());
        if (ImGui::MenuItem("Open Image...", "Ctrl+O")) {
            /// File-type filter list shown in the native dialog.
            static const SDL_DialogFileFilter filters[] = {
                {"Image files", "png;jpg;jpeg;bmp;tga;gif;webp"},
                {"All files", "*"},
            };

            /**
             * SDL3 shows the OS-native file picker asynchronously.
             * When the user confirms, file_dialog_callback() fires on
             * the main thread, populating m_pending_paths.
             */
            SDL_ShowOpenFileDialog(file_dialog_callback, this, m_window,
                                   filters, 2, nullptr, /*allow_many=*/true);
        }
        ImGui::EndDisabled(); /// Re-enable widgets after the Open button.

        /// "Open Online..." also respects the cap.
        ImGui::BeginDisabled(m_viewer.is_at_capacity());
        if (ImGui::MenuItem("Open Online...")) {
            m_url_buf[0] = '\0';     /// Clear the text buffer.
            m_show_url_popup = true; /// Trigger OpenPopup on the next section.
        }
        ImGui::EndDisabled();

        // --- Recent sub-menu -------------------------------------------------

        if (!m_history.empty() && ImGui::BeginMenu("Recent")) {
            constexpr int k_max_shown = 20; /// Maximum history items shown at once.
            int shown = 0;

            for (const auto &hentry : m_history) {
                if (shown >= k_max_shown)
                    break;
                ++shown;

                /**
                 * Build the menu-item label:
                 *   [file]  cat.jpg              (filename only for local files)
                 *   [url]   https://example...   (truncated to 60 chars)
                 */
                std::string label;
                if (hentry.kind == "file") {
                    label = "[file]  ";
                    label += std::filesystem::path(hentry.source).filename().string();
                } else {
                    label = "[url]   ";
                    label += hentry.source.size() > 60
                                 ? hentry.source.substr(0, 57) + "..."
                                 : hentry.source;
                }

                /// Disable the item if the viewer is already at capacity.
                ImGui::BeginDisabled(m_viewer.is_at_capacity());
                if (ImGui::MenuItem(label.c_str())) {
                    /// Re-open the item via the appropriate loading path.
                    if (hentry.kind == "file") {
                        m_pending_paths.push_back(hentry.source);
                        m_has_pending_path = true;
                    } else {
                        m_pending_urls.push_back(hentry.source);
                    }
                }
                ImGui::EndDisabled();

                /// Show the full source and timestamp as a tooltip on hover.
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n%s",
                                      hentry.source.c_str(),
                                      hentry.opened_at.c_str());
            }

            /// If the history is longer than k_max_shown, indicate the overflow.
            if (shown < static_cast<int>(m_history.size())) {
                ImGui::Separator();
                ImGui::TextDisabled("(%zu more not shown)",
                                    m_history.size() - static_cast<size_t>(shown));
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Clear History"))
                m_history.clear(); /// Wipe the in-memory history (persisted on next save).

            ImGui::EndMenu();
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Quit", "Alt+F4"))
            request_quit = true; /// The main loop checks this flag.

        ImGui::EndMenu();
    }

    // --- View menu -----------------------------------------------------------

    if (ImGui::BeginMenu("View")) {
        /// Style editor toggle — only shown when one is attached.
        if (m_style_editor)
            ImGui::MenuItem("Style Editor", nullptr, &m_style_editor->IsOpen);

        if (m_show_demo_window)
            ImGui::MenuItem("Demo Window", nullptr, m_show_demo_window);

        if (m_show_another_window)
            ImGui::MenuItem("Another Window", nullptr, m_show_another_window);

        /**
         * Delegate image toggle items to the panel.
         * Each item lets the user re-show a minimised or closed window.
         */
        if (m_viewer.count() > 0) {
            ImGui::Separator();
            m_viewer.build_view_menu_items();
        }

        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();

    // -------------------------------------------------------------------------
    // Step 5 — URL popup
    // -------------------------------------------------------------------------

    /**
     * m_show_url_popup acts as a one-shot flag: we call OpenPopup here,
     * then immediately clear the flag so the popup opens exactly once.
     * OpenPopup must be called OUTSIDE of BeginPopupModal.
     */
    if (m_show_url_popup) {
        ImGui::OpenPopup("Open Online");
        m_show_url_popup = false;
    }

    if (ImGui::BeginPopupModal("Open Online", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter image URL:");

        /// Fixed-width input field; EnterReturnsTrue lets the user press Enter to confirm.
        ImGui::SetNextItemWidth(480.0f);
        const bool enter_pressed = ImGui::InputText(
            "##url", m_url_buf.data(), m_url_buf.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);

        /// Keep focus on the text field by default when the popup opens.
        ImGui::SetItemDefaultFocus();

        const bool ok = ImGui::Button("Open", ImVec2(100.0f, 0.0f)) || enter_pressed;
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel", ImVec2(100.0f, 0.0f));

        if (ok && m_url_buf[0] != '\0') {
            /// Queue the entered URL for download in the next Build() call.
            m_pending_urls.emplace_back(m_url_buf.data());
            m_url_buf[0] = '\0'; /// Clear the buffer for next use.
            ImGui::CloseCurrentPopup();
        } else if (cancel || (enter_pressed && m_url_buf[0] == '\0')) {
            /// Cancel: close without queuing anything.
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // -------------------------------------------------------------------------
    // Step 6 — image viewer windows
    // -------------------------------------------------------------------------

    /**
     * Delegate all per-image ImGui window rendering to ImageViewerPanel.
     * This draws one window per open image with zoom, pan, and black bg.
     */
    m_viewer.draw_windows();
}