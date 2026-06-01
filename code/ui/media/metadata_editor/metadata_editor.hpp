#pragma once

#include "pch.hpp"

/// ImGui window for viewing and editing audio/video file metadata tags.
///
/// Uses TagLib's PropertyMap API — the same key names (TITLE, ARTIST, ALBUM, …)
/// that Dolphin / KDE apps use, so tags written here are immediately visible in
/// the file manager without any conversion.
///
/// Typical usage:
///   MetadataEditor m_metadata_editor;           // owned by MainMenuBar
///   m_metadata_editor.Open(path);               // from context menu
///   m_metadata_editor.Draw();                   // called every frame
class MetadataEditor {
public:
    MetadataEditor();

    MetadataEditor(const MetadataEditor &) = delete;
    MetadataEditor &operator=(const MetadataEditor &) = delete;

    /// Open the editor for a single file.  Loads existing tags into buffers.
    /// Empty buffer on Save = remove that tag from the file.
    void Open(const std::filesystem::path &path);

    /// Open the editor for multiple files at once.
    /// All fields start empty — only fields you fill will be written to every
    /// file.  Unfilled fields preserve each file's individual existing value.
    void OpenMultiple(const std::vector<std::filesystem::path> &paths);

    /// Draw the ImGui window.  Call every frame between NewFrame and Render.
    void Draw();

    [[nodiscard]] bool IsOpen() const noexcept;

private:
    // ─── State ────────────────────────────────────────────────────────────────
    bool                                  m_open;
    bool                                  m_dirty;   ///< unsaved changes
    bool                                  m_multi;   ///< multi-file mode
    std::filesystem::path                 m_path;    ///< single-file path
    std::vector<std::filesystem::path>    m_paths;   ///< multi-file paths

    // ─── Editable tag buffers (UTF-8) ─────────────────────────────────────────
    std::array<char, 512>   m_title;
    std::array<char, 512>   m_artist;
    std::array<char, 512>   m_album_artist;
    std::array<char, 512>   m_album;
    std::array<char, 256>   m_track_number;  ///< "3" or "3/12"
    std::array<char, 256>   m_disc_number;   ///< "1" or "1/2"
    std::array<char, 32>    m_date;          ///< "2024" or "2024-07-04"
    std::array<char, 256>   m_genre;
    std::array<char, 256>   m_composer;
    std::array<char, 1024>  m_comment;

    // ─── Dolphin-compatible xattr tags ───────────────────────────────────────
    std::vector<std::string>  m_xattr_tags;           ///< working set of file tags
    std::vector<std::string>  m_xattr_tags_original;  ///< state at last load/save (multi-file diff)
    std::array<char, 128>     m_tag_input;             ///< new-tag text-input buffer

    // ─── Audio properties (read-only) ────────────────────────────────────────
    int   m_duration_sec;   ///< total duration in seconds, -1 if unknown
    int   m_bitrate;        ///< kbps, -1 if unknown
    int   m_sample_rate;    ///< Hz, -1 if unknown
    int   m_channels;       ///< -1 if unknown

    // ─── Internal helpers ─────────────────────────────────────────────────────
    void ClearBuffers();
    void LoadFromFile();
    /// Single-file save: empty buffer removes the tag; non-empty sets it.
    void SaveToFile();
    /// Multi-file apply: only non-empty buffers are written; empty = keep existing.
    void ApplyToFile(const std::filesystem::path &path);

    /// Snap a TagLib StringList's first value into a fixed char buffer.
    /// Clears the buffer if the key is absent.
    template <std::size_t N>
    static void CopyProp(const std::string &value, std::array<char, N> &buf);

    /// Load user.xdg.tags into m_xattr_tags / m_xattr_tags_original.
    /// Single-file: reads from m_path. Multi-file: computes intersection.
    void LoadXattrTags();
    /// Multi-file: applies add/remove diff to one file's xattr tags.
    void ApplyXattrTagsToFile(const std::filesystem::path &path);

    static std::vector<std::string> ReadXattrTags(const std::filesystem::path &path);
    static void WriteXattrTags(const std::filesystem::path &path,
                                const std::vector<std::string> &tags);
};
