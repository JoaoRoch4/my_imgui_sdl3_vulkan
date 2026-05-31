/**
 * @file media_history_manager.cpp
 * @brief Manages the in-memory recently-opened-media history and its TOML persistence.
 *
 * Extracted from MainMenuBar to give the class a single responsibility:
 * knowing what was opened, when, and how to save / load that information.
 *
 * All mutation methods (push, erase, clear) immediately synchronise the
 * companion OpenedFilesWindow UI and write through to the TOML file so that
 * the history survives crashes without a clean shutdown.
 */

#include "media_history_manager.hpp"
#include "image_downloader.hpp"
#include "opened_files_window.hpp"
#include "video_playback_mode.hpp"
#include "window_state_toml.hpp"

#include <ctime>

namespace {

std::string url_path_filename(const std::string &url)
{
    const std::string clean = url.substr(0, url.find('?'));
    return std::filesystem::path(clean).filename().string();
}

bool should_refresh_url_title(const WindowStateToml::ImageHistoryEntry &entry,
                              const std::string &parsed_title)
{
    if (parsed_title.empty() || entry.title == parsed_title)
        return false;

    if (entry.title.empty() || entry.title == "online_image")
        return true;

    if (!entry.cached_path.empty() &&
        entry.title == std::filesystem::path(entry.cached_path).filename().string())
        return true;

    return entry.title == url_path_filename(entry.source);
}

} // namespace

// ============================================================================
// Constructor
// ============================================================================

/**
 * @brief Default constructor — both members default-initialise to empty.
 */
MediaHistoryManager::MediaHistoryManager()
    : m_entries{}    // empty vector; filled by apply() at startup
    , m_state_path{} // empty path; set by set_state_path() before first persist()
{
}

// ============================================================================
// Configuration
// ============================================================================

/**
 * @brief Set the filesystem path used for all TOML read/write operations.
 *
 * Must be called before persist() or apply() will do anything useful.
 * Typically called from MainMenuBar::SetStatePath().
 *
 * @param path  Absolute path to the application state TOML file.
 */
void MediaHistoryManager::set_state_path(const std::filesystem::path &path)
{
    m_state_path = path; // store for every subsequent persist() call
}

// ============================================================================
// Mutation
// ============================================================================

/**
 * @brief Insert a new entry at the front of the history list, deduplicating by source.
 *
 * If a matching source already exists it is removed first so the newest
 * access is always at index 0.  The opened-files window is synchronised and
 * the change is written to disk immediately so it survives crashes.
 *
 * @param source       Absolute file path or full URL string.
 * @param kind         Literal string "file" or "url".
 * @param title        Human-readable display name shown in menus.
 * @param files_window The OpenedFilesWindow that mirrors this list in the UI.
 */
void MediaHistoryManager::push(const std::string &source,
                               const std::string &kind,
                               const std::string &title,
                               OpenedFilesWindow &files_window)
{
    // Build the ISO-8601 timestamp string for this access moment.
    std::array<char, 20> ts{};
    current_timestamp(ts); // writes "YYYY-MM-DDTHH:MM:SS\0" into ts

    WindowStateToml::ImageHistoryEntry preserved_entry{};
    bool had_existing_entry = false;

    if (const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&source](const WindowStateToml::ImageHistoryEntry &entry) {
            return entry.source == source;
        }); it != m_entries.end()) {
        preserved_entry = *it;
        had_existing_entry = true;
    }

    // Remove any existing entry with the same source so there are no duplicates.
    std::erase_if(m_entries, [&source](const WindowStateToml::ImageHistoryEntry &e) {
        return e.source == source; // match on the raw path/url string
    });

    // Populate a new entry and insert it at position 0 (most recent).
    WindowStateToml::ImageHistoryEntry entry{};
    entry.source    = source;       // full path or url
    entry.kind      = kind;         // "file" | "url"
    entry.opened_at = ts.data();    // ISO-8601 wall-clock timestamp
    entry.title     = title;        // human-readable name
    if (had_existing_entry) {
        entry.thumbnail_path = preserved_entry.thumbnail_path;
        entry.cached_path = preserved_entry.cached_path;
        entry.playback_mode = preserved_entry.playback_mode;
        entry.hwdec_enabled = preserved_entry.hwdec_enabled;
        entry.resume_position_seconds = preserved_entry.resume_position_seconds;
        entry.startup_restore = preserved_entry.startup_restore;
    }
    m_entries.insert(m_entries.begin(), std::move(entry)); // newest at front

    files_window.sync_history(m_entries); // keep the companion list panel up to date
    persist();                            // write immediately — crash-safe
}

void MediaHistoryManager::replace_with_saved_file(const std::string &source,
                                                  const std::filesystem::path &saved_path,
                                                  OpenedFilesWindow &files_window)
{
    const std::string saved_source = saved_path.string();

    std::erase_if(m_entries, [&saved_source, &source](const WindowStateToml::ImageHistoryEntry &e) {
        return e.source == saved_source && e.source != source;
    });

    auto it = std::find_if(m_entries.begin(), m_entries.end(), [&source](const WindowStateToml::ImageHistoryEntry &e) {
        return e.source == source;
    });

    if (it == m_entries.end()) {
        push(saved_source,
             "file",
             saved_path.filename().string(),
             files_window);
        return;
    }

    it->source = saved_source;
    it->kind = "file";
    it->title = saved_path.filename().string();
    it->cached_path.clear();

    files_window.sync_history(m_entries);
    persist();
}

void MediaHistoryManager::set_hwdec_enabled(const std::string &source,
                                            bool enabled,
                                            OpenedFilesWindow &files_window,
                                            int resume_position_seconds)
{
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [&source](const WindowStateToml::ImageHistoryEntry &entry) {
        return entry.source == source;
    });

    if (it == m_entries.end())
        return;

    const bool changed_hwdec = it->hwdec_enabled != enabled;
    const bool changed_resume = resume_position_seconds >= 0 &&
                                it->resume_position_seconds != resume_position_seconds;

    if (!changed_hwdec && !changed_resume)
        return;

    it->hwdec_enabled = enabled;
    if (enabled) {
        if (it->playback_mode < 0)
            it->playback_mode = static_cast<int>(VideoPlaybackMode::NvdecMpv);
    } else {
        it->playback_mode = static_cast<int>(VideoPlaybackMode::SwMpv);
    }
    if (resume_position_seconds >= 0)
        it->resume_position_seconds = resume_position_seconds;
    files_window.sync_history(m_entries);
    persist();
}

void MediaHistoryManager::set_playback_mode(const std::string &source,
                                            int mode,
                                            OpenedFilesWindow &files_window,
                                            int resume_position_seconds)
{
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [&source](const WindowStateToml::ImageHistoryEntry &entry) {
        return entry.source == source;
    });

    if (it == m_entries.end())
        return;

    const int next_mode = sanitize_video_playback_mode(mode);
    const bool next_hwdec = mode_uses_hwdec(next_mode);
    const bool changed_mode = it->playback_mode != next_mode;
    const bool changed_hwdec = it->hwdec_enabled != next_hwdec;
    const bool changed_resume = resume_position_seconds >= 0 &&
                                it->resume_position_seconds != resume_position_seconds;

    if (!changed_mode && !changed_hwdec && !changed_resume)
        return;

    it->playback_mode = next_mode;
    it->hwdec_enabled = next_hwdec;
    if (resume_position_seconds >= 0)
        it->resume_position_seconds = resume_position_seconds;

    files_window.sync_history(m_entries);
    persist();
}

/**
 * @brief Remove all entries whose source matches the given string.
 *
 * Normally only one entry matches; the erase-if removes all to be safe.
 * Synchronises the companion UI and persists the deletion immediately.
 *
 * @param source       The file path or URL to remove.
 * @param files_window Companion panel that must reflect the updated list.
 */
void MediaHistoryManager::erase(const std::string &source, OpenedFilesWindow &files_window)
{
    // Remove every entry whose source field matches — usually exactly one.
    std::erase_if(m_entries, [&source](const WindowStateToml::ImageHistoryEntry &e) {
        return e.source == source;
    });

    files_window.sync_history(m_entries); // reflect the removal in the UI panel
    persist();                            // commit the deletion to the TOML file
}

/**
 * @brief Wipe the entire history list.
 *
 * The companion UI is notified and the empty list is persisted so the
 * cleared state is reflected on the next application startup.
 *
 * @param files_window Companion panel that must reflect the empty list.
 */
void MediaHistoryManager::clear(OpenedFilesWindow &files_window)
{
    m_entries.clear();                    // drop all in-memory entries at once
    files_window.sync_history(m_entries); // show the empty state in the panel
    persist();                            // commit the cleared history to TOML
}

// ============================================================================
// Persistence — load / save
// ============================================================================

/**
 * @brief Populate the history from a previously loaded TOML state object.
 *
 * Also backfills human-readable titles for older URL entries that were
 * saved before the title-extraction logic existed (e.g. "online_image").
 *
 * @param state        Loaded TOML state — acts as the source of truth on disk.
 * @param files_window Companion panel synchronised after the rebuild.
 */
void MediaHistoryManager::apply(const WindowStateToml &state, OpenedFilesWindow &files_window)
{
    // Replace the in-memory list wholesale with whatever was saved to disk.
    m_entries = state.image_history;

    // Backfill titles for URL entries that have a stale or empty name.
    bool changed = false; // track whether any title was improved
    for (auto &h : m_entries) {
        // Only URL-kind entries ever end up with generic titles.
        if (h.kind != "url")
            continue;

        // Try to extract a better title from the URL's path/query components.
        const std::string parsed = ImageDownloader::title_from_url(h.source);
        if (should_refresh_url_title(h, parsed)) {
            h.title = parsed; // update to the freshly extracted title
            changed = true;   // remember we have to write this back to disk
        }
    }

    // Keep the companion UI in sync with any repaired titles immediately.
    WindowStateToml synced_state = state;
    synced_state.image_history = m_entries;
    files_window.apply_history(synced_state);

    // If any titles were improved, write the corrected entries to disk now.
    if (changed)
        persist();
}

/**
 * @brief Serialise the current history into a WindowStateToml for saving.
 *
 * Caps at 100 entries to keep the TOML file a manageable size.
 *
 * @param state  Output TOML state object; its image_history field is replaced.
 */
void MediaHistoryManager::export_to(WindowStateToml *state) const
{
    constexpr size_t k_max = 100; // maximum entries written to the TOML file

    // Clamp to the actual vector size to avoid overshooting with begin() + count.
    const size_t count = std::min(k_max, m_entries.size());

    // Copy only the first 'count' entries (newest first) into the output state.
    state->image_history = std::vector<WindowStateToml::ImageHistoryEntry>(
        m_entries.begin(),
        m_entries.begin() + static_cast<std::ptrdiff_t>(count));
}

/**
 * @brief Write the current history to the TOML state file immediately.
 *
 * Loads the existing on-disk state first so that unrelated fields (window
 * positions, config flags, etc.) are preserved — only the image_history
 * section is replaced.
 *
 * Does nothing if no state path has been provided yet.
 */
void MediaHistoryManager::persist() const
{
    // Bail out if the caller has not yet provided a state file path.
    if (m_state_path.empty())
        return;

    // Load the current on-disk state to preserve all non-history fields.
    WindowStateToml state{};
    LoadWindowStateToml(m_state_path, state); // fills state from TOML on disk

    // Overwrite only the history section with the current in-memory list.
    export_to(&state);

    // Flush the merged state back to disk atomically via TOML serialisation.
    SaveWindowStateToml(m_state_path, state);
}

// ============================================================================
// Accessors
// ============================================================================

/**
 * @brief Read-only view of the history entries (newest at index 0).
 * @return Const reference to the internal vector — no copy made.
 */
const std::vector<WindowStateToml::ImageHistoryEntry> &MediaHistoryManager::entries() const
{
    return m_entries; // direct reference; caller must not store it beyond this call
}

/**
 * @brief Mutable view of the history entries.
 *
 * Used when a caller needs to update a field on an existing entry in-place
 * (e.g. setting cached_path after a background download completes).
 * The caller is responsible for calling persist() afterward if the change
 * must survive a restart.
 *
 * @return Mutable reference to the internal vector — no copy made.
 */
std::vector<WindowStateToml::ImageHistoryEntry> &MediaHistoryManager::entries()
{
    return m_entries; // direct reference; caller must not store it beyond this call
}

// ============================================================================
// Private helpers
// ============================================================================

/**
 * @brief Write the current local time as "YYYY-MM-DDTHH:MM:SS" into dst.
 *
 * Uses localtime_r (POSIX thread-safe variant) to avoid data races if this
 * is ever called from multiple threads.
 *
 * @param dst  20-element array; receives exactly 19 chars plus a null terminator.
 */
void MediaHistoryManager::current_timestamp(std::array<char, 20> &dst)
{
    // Capture the current wall-clock instant as a POSIXtime_t (seconds since epoch).
    const auto now_c = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());

    // Break the epoch value into human-readable calendar fields (thread-safe form).
    struct tm tm_val{};
    localtime_r(&now_c, &tm_val); // POSIX reentrant — safe from multiple call sites

    // Format into the 20-byte fixed-size buffer: exactly 19 printable chars + '\0'.
    std::strftime(dst.data(), dst.size(), "%Y-%m-%dT%H:%M:%S", &tm_val);
}