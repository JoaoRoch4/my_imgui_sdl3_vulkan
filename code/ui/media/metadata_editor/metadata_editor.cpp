#include "pch.hpp" // NOLINT

#include "metadata_editor.hpp"

// ─── constructor ─────────────────────────────────────────────────────────────

MetadataEditor::MetadataEditor()
    : m_open(false)
    , m_dirty(false)
    , m_multi(false)
    , m_duration_sec(-1)
    , m_bitrate(-1)
    , m_sample_rate(-1)
    , m_channels(-1)
{
    m_title.fill(0);
    m_artist.fill(0);
    m_album_artist.fill(0);
    m_album.fill(0);
    m_track_number.fill(0);
    m_disc_number.fill(0);
    m_date.fill(0);
    m_genre.fill(0);
    m_composer.fill(0);
    m_comment.fill(0);
    m_tag_input.fill(0);
}

// ─── ClearBuffers ─────────────────────────────────────────────────────────────

void MetadataEditor::ClearBuffers()
{
    m_title.fill(0);       m_artist.fill(0);       m_album_artist.fill(0);
    m_album.fill(0);       m_track_number.fill(0); m_disc_number.fill(0);
    m_date.fill(0);        m_genre.fill(0);        m_composer.fill(0);
    m_comment.fill(0);     m_tag_input.fill(0);
}

// ─── Open ────────────────────────────────────────────────────────────────────

void MetadataEditor::Open(const std::filesystem::path &path)
{
    m_multi = false;
    m_path  = path;
    m_open  = true;
    m_dirty = false;
    LoadFromFile();
}

// ─── OpenMultiple ─────────────────────────────────────────────────────────────

void MetadataEditor::OpenMultiple(const std::vector<std::filesystem::path> &paths)
{
    if (paths.empty()) return;
    m_multi = true;
    m_paths = paths;
    m_open  = true;
    m_dirty = false;
    // All buffers empty — user fills only what they want to apply to every file.
    ClearBuffers();
    m_duration_sec = -1;
    m_bitrate      = -1;
    m_sample_rate  = -1;
    m_channels     = -1;
    LoadXattrTags();
}

// ─── IsOpen ──────────────────────────────────────────────────────────────────

bool MetadataEditor::IsOpen() const noexcept { return m_open; }

// ─── ReadXattrTags / WriteXattrTags ──────────────────────────────────────────

std::vector<std::string> MetadataEditor::ReadXattrTags(const std::filesystem::path &path)
{
    std::vector<std::string> result;

    const ssize_t sz = getxattr(path.c_str(), "user.xdg.tags", nullptr, 0);
    if (sz <= 0)
        return result;

    std::string raw(static_cast<std::size_t>(sz), '\0');
    if (getxattr(path.c_str(), "user.xdg.tags", raw.data(), raw.size()) < 0)
        return result;

    // Parse comma-separated list; trim whitespace from each token
    std::size_t start = 0;
    while (start < raw.size()) {
        std::size_t end = raw.find(',', start);
        if (end == std::string::npos)
            end = raw.size();

        std::string tag = raw.substr(start, end - start);

        while (!tag.empty() && (tag.front() == ' ' || tag.front() == '\t'))
            tag.erase(tag.begin());
        while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t'))
            tag.pop_back();

        if (!tag.empty())
            result.push_back(std::move(tag));

        start = end + 1;
    }
    return result;
}

void MetadataEditor::WriteXattrTags(const std::filesystem::path &path,
                                     const std::vector<std::string> &tags)
{
    if (tags.empty()) {
        removexattr(path.c_str(), "user.xdg.tags");
        return;
    }

    std::string raw;
    for (std::size_t i = 0; i < tags.size(); ++i) {
        if (i > 0)
            raw += ',';
        raw += tags[i];
    }
    setxattr(path.c_str(), "user.xdg.tags", raw.data(), raw.size(), 0);
}

// ─── LoadXattrTags ───────────────────────────────────────────────────────────

void MetadataEditor::LoadXattrTags()
{
    m_tag_input.fill(0);

    if (m_multi) {
        if (m_paths.empty()) {
            m_xattr_tags.clear();
            m_xattr_tags_original.clear();
            return;
        }
        // Start with the first file's tags, then keep only those present in every file
        m_xattr_tags = ReadXattrTags(m_paths[0]);
        for (std::size_t i = 1; i < m_paths.size() && !m_xattr_tags.empty(); ++i) {
            const auto other = ReadXattrTags(m_paths[i]);
            m_xattr_tags.erase(
                std::remove_if(m_xattr_tags.begin(), m_xattr_tags.end(),
                    [&](const std::string &t) {
                        return std::find(other.begin(), other.end(), t) == other.end();
                    }),
                m_xattr_tags.end());
        }
    } else {
        m_xattr_tags = ReadXattrTags(m_path);
    }

    m_xattr_tags_original = m_xattr_tags;
}

// ─── ApplyXattrTagsToFile ─────────────────────────────────────────────────────

void MetadataEditor::ApplyXattrTagsToFile(const std::filesystem::path &path)
{
    auto tags = ReadXattrTags(path);

    // Tags that were added since last load: add to file if not already present
    for (const auto &t : m_xattr_tags) {
        const bool was_original = std::find(m_xattr_tags_original.begin(),
                                            m_xattr_tags_original.end(), t)
                                  != m_xattr_tags_original.end();
        if (!was_original && std::find(tags.begin(), tags.end(), t) == tags.end())
            tags.push_back(t);
    }

    // Tags that were removed from the working set: remove from file
    for (const auto &t : m_xattr_tags_original) {
        const bool still_present = std::find(m_xattr_tags.begin(),
                                             m_xattr_tags.end(), t)
                                   != m_xattr_tags.end();
        if (!still_present)
            tags.erase(std::remove(tags.begin(), tags.end(), t), tags.end());
    }

    WriteXattrTags(path, tags);
}

// ─── CopyProp ────────────────────────────────────────────────────────────────
template <std::size_t N>
void MetadataEditor::CopyProp(const std::string &value, std::array<char, N> &buf)
{
    buf.fill(0);
    const std::size_t len = std::min(value.size(), N - 1);
    std::memcpy(buf.data(), value.data(), len);
}

// ─── LoadFromFile ─────────────────────────────────────────────────────────────

void MetadataEditor::LoadFromFile()
{
    // Reset audio properties
    m_duration_sec = -1;
    m_bitrate      = -1;
    m_sample_rate  = -1;
    m_channels     = -1;

    ClearBuffers();

    // xattr tags are independent of TagLib \u2014 load them regardless of file type
    LoadXattrTags();

    TagLib::FileRef ref(m_path.c_str());
    if (ref.isNull())
        return;

    // ─── Audio properties ────────────────────────────────────────────────────
    if (const TagLib::AudioProperties *ap = ref.audioProperties(); ap) {
        m_duration_sec = ap->lengthInSeconds();
        m_bitrate      = ap->bitrate();
        m_sample_rate  = ap->sampleRate();
        m_channels     = ap->channels();
    }

    // ─── Tags via PropertyMap (Dolphin-compatible key names) ─────────────────
    if (TagLib::Tag *tag = ref.tag(); tag) {
        TagLib::PropertyMap props = tag->properties();

        auto get = [&](const char *key) -> std::string {
            const TagLib::StringList &list = props[key];
            if (list.isEmpty())
                return {};
            return list.front().to8Bit(true); // UTF-8
        };

        CopyProp(get("TITLE"),       m_title);
        CopyProp(get("ARTIST"),      m_artist);
        CopyProp(get("ALBUMARTIST"), m_album_artist);
        CopyProp(get("ALBUM"),       m_album);
        CopyProp(get("TRACKNUMBER"), m_track_number);
        CopyProp(get("DISCNUMBER"),  m_disc_number);
        CopyProp(get("DATE"),        m_date);
        CopyProp(get("GENRE"),       m_genre);
        CopyProp(get("COMPOSER"),    m_composer);
        CopyProp(get("COMMENT"),     m_comment);
    }
}

// ─── SaveToFile ──────────────────────────────────────────────────────────────

void MetadataEditor::SaveToFile()
{
    // xattr tags are independent of TagLib — always write them
    WriteXattrTags(m_path, m_xattr_tags);
    m_xattr_tags_original = m_xattr_tags;

    TagLib::FileRef ref(m_path.c_str());
    if (ref.isNull() || !ref.tag()) {
        m_dirty = false;
        return;
    }

    TagLib::PropertyMap props = ref.tag()->properties();

    auto setOrRemove = [&](const char *key, const char *buf) {
        std::string_view sv(buf);
        if (sv.empty()) {
            props.erase(key);
        } else {
            TagLib::StringList list;
            list.append(TagLib::String(buf, TagLib::String::UTF8));
            props[key] = list;
        }
    };

    setOrRemove("TITLE",       m_title.data());
    setOrRemove("ARTIST",      m_artist.data());
    setOrRemove("ALBUMARTIST", m_album_artist.data());
    setOrRemove("ALBUM",       m_album.data());
    setOrRemove("TRACKNUMBER", m_track_number.data());
    setOrRemove("DISCNUMBER",  m_disc_number.data());
    setOrRemove("DATE",        m_date.data());
    setOrRemove("GENRE",       m_genre.data());
    setOrRemove("COMPOSER",    m_composer.data());
    setOrRemove("COMMENT",     m_comment.data());

    ref.tag()->setProperties(props);
    ref.save();
    m_dirty = false;
}

// ─── ApplyToFile (multi-file: only non-empty buffers are written) ───────────────────

void MetadataEditor::ApplyToFile(const std::filesystem::path &path)
{
    // xattr tags are independent of TagLib — always apply them
    ApplyXattrTagsToFile(path);

    TagLib::FileRef ref(path.c_str());
    if (ref.isNull() || !ref.tag())
        return;

    TagLib::PropertyMap props = ref.tag()->properties();

    // Only overwrite keys whose buffer is non-empty; empty = keep existing value.
    auto setIfFilled = [&](const char *key, const char *buf) {
        if (std::string_view(buf).empty())
            return;
        TagLib::StringList list;
        list.append(TagLib::String(buf, TagLib::String::UTF8));
        props[key] = list;
    };

    setIfFilled("TITLE",       m_title.data());
    setIfFilled("ARTIST",      m_artist.data());
    setIfFilled("ALBUMARTIST", m_album_artist.data());
    setIfFilled("ALBUM",       m_album.data());
    setIfFilled("TRACKNUMBER", m_track_number.data());
    setIfFilled("DISCNUMBER",  m_disc_number.data());
    setIfFilled("DATE",        m_date.data());
    setIfFilled("GENRE",       m_genre.data());
    setIfFilled("COMPOSER",    m_composer.data());
    setIfFilled("COMMENT",     m_comment.data());

    ref.tag()->setProperties(props);
    ref.save();
    ApplyXattrTagsToFile(path);
}

// ─── Draw ────────────────────────────────────────────────────────────────────

void MetadataEditor::Draw()
{
    if (!m_open)
        return;

    // ─── Window title ──────────────────────────────────────────────────────────
    std::string window_title;
    if (m_multi)
        window_title = "Edit Tags — " + std::to_string(m_paths.size()) + " files";
    else
        window_title = "Edit Tags — " + m_path.filename().string();

    ImGui::SetNextWindowSize(ImVec2(560.f, 580.f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(window_title.c_str(), &m_open)) {
        ImGui::End();
        return;
    }

    // ─── Header ───────────────────────────────────────────────────────────────
    if (m_multi) {
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.f, 1.f),
            "Editing %zu files. Only filled fields will be written.",
            m_paths.size());
        ImGui::TextDisabled("Empty fields preserve each file's existing value.");
    } else {
        ImGui::TextDisabled("%s", m_path.parent_path().c_str());
        // ─── Audio properties (single-file only) ──────────────────────────────
        if (m_duration_sec >= 0 || m_bitrate >= 0) {
            ImGui::Separator();
            ImGui::BeginDisabled();
            if (m_duration_sec >= 0) {
                const int h = m_duration_sec / 3600;
                const int m_min = (m_duration_sec % 3600) / 60;
                const int s = m_duration_sec % 60;
                if (h > 0)
                    ImGui::Text("Duration: %d:%02d:%02d", h, m_min, s);
                else
                    ImGui::Text("Duration: %d:%02d", m_min, s);
                ImGui::SameLine();
            }
            if (m_bitrate > 0)     { ImGui::Text("  %d kbps", m_bitrate);    ImGui::SameLine(); }
            if (m_sample_rate > 0) { ImGui::Text("  %d Hz",   m_sample_rate); ImGui::SameLine(); }
            if (m_channels > 0)    { ImGui::Text("  %dch",    m_channels); }
            ImGui::EndDisabled();
        }
    }
    ImGui::Separator();

    // ─── Field table ──────────────────────────────────────────────────────────
    auto field = [&](const char *label, auto &buf, bool multiline = false) -> bool {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        bool changed = false;
        const std::string id = std::string("##") + label;
        if (multiline) {
            changed = ImGui::InputTextMultiline(
                id.c_str(), buf.data(), buf.size(),
                ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3));
        } else {
            changed = ImGui::InputText(id.c_str(), buf.data(), buf.size());
        }
        return changed;
    };

    constexpr ImGuiTableFlags table_flags =
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;

    if (ImGui::BeginTable("##meta_fields", 2, table_flags)) {
        ImGui::TableSetupColumn("Label",
            ImGuiTableColumnFlags_WidthFixed,
            ImGui::CalcTextSize("Album Artist").x + 8.f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        if (field("Title",        m_title))        m_dirty = true;
        if (field("Artist",       m_artist))       m_dirty = true;
        if (field("Album Artist", m_album_artist)) m_dirty = true;
        if (field("Album",        m_album))        m_dirty = true;
        if (field("Track",        m_track_number)) m_dirty = true;
        if (field("Disc",         m_disc_number))  m_dirty = true;
        if (field("Date",         m_date))         m_dirty = true;
        if (field("Genre",        m_genre))        m_dirty = true;
        if (field("Composer",     m_composer))     m_dirty = true;
        if (field("Comment",      m_comment, /*multiline=*/true)) m_dirty = true;

        ImGui::EndTable();
    }

    // ─── Tags (Dolphin user.xdg.tags) ────────────────────────────────────────
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Tags:");
    ImGui::SameLine();
    if (m_multi)
        ImGui::TextDisabled("(common to all \xe2\x80\x94 changes apply to every selected file)");
    else
        ImGui::TextDisabled("(user.xdg.tags \xe2\x80\x94 visible in Dolphin)");

    // Chip row — each chip is a button "label ×"; click removes the tag
    {
        const float avail   = ImGui::GetContentRegionAvail().x;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        float       row_x   = 0.f;

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(6.f, 2.f));

        for (std::size_t i = 0; i < m_xattr_tags.size(); ) {
            const std::string &t   = m_xattr_tags[i];
            const std::string  lbl = t + "  \xc3\x97";  // × U+00D7

            const float w = ImGui::CalcTextSize(lbl.c_str()).x
                          + ImGui::GetStyle().FramePadding.x * 2.f;

            if (row_x > 0.f) {
                if (row_x + spacing + w <= avail)
                    ImGui::SameLine();
                else
                    row_x = 0.f;
            }

            ImGui::PushID(static_cast<int>(i));
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.45f, 0.70f, 0.75f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.55f, 0.85f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.10f, 0.35f, 0.60f, 1.00f));

            if (ImGui::Button(lbl.c_str())) {
                m_xattr_tags.erase(m_xattr_tags.begin() + static_cast<std::ptrdiff_t>(i));
                m_dirty = true;
                ImGui::PopStyleColor(3);
                ImGui::PopID();
                row_x = 0.f;
                continue;
            }

            ImGui::PopStyleColor(3);
            ImGui::PopID();
            row_x += (row_x > 0.f ? spacing : 0.f) + w;
            ++i;
        }

        ImGui::PopStyleVar(2);
    }

    // Add-tag input
    ImGui::SetNextItemWidth(180.f);
    const bool enter_pressed = ImGui::InputText(
        "##new_tag", m_tag_input.data(), m_tag_input.size(),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool add_clicked = ImGui::Button("Add##add_tag");

    if (enter_pressed || add_clicked) {
        std::string_view raw(m_tag_input.data());
        const std::size_t s = raw.find_first_not_of(" \t,");
        const std::size_t e = raw.find_last_not_of(" \t,");
        if (s != std::string_view::npos) {
            std::string new_tag(raw.substr(s, e - s + 1));
            if (std::find(m_xattr_tags.begin(), m_xattr_tags.end(), new_tag)
                    == m_xattr_tags.end()) {
                m_xattr_tags.push_back(std::move(new_tag));
                m_dirty = true;
            }
        }
        m_tag_input.fill(0);
        if (enter_pressed)
            ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::Separator();

    // ─── Action buttons ───────────────────────────────────────────────────────
    const bool dirty_now = m_dirty;  // capture before any button modifies it
    if (!dirty_now)
        ImGui::BeginDisabled();
    if (ImGui::Button("Save")) {
        if (m_multi) {
            for (const auto &p : m_paths)
                ApplyToFile(p);
            m_dirty = false;
        } else {
            SaveToFile();
        }
    }
    if (!dirty_now)
        ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        m_dirty = false;
        if (m_multi) {
            ClearBuffers();     // reset audio-metadata buffers to "don't touch"
            LoadXattrTags();    // re-compute xattr tag intersection
        } else {
            LoadFromFile();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        m_open  = false;
        m_dirty = false;
    }

    if (m_dirty) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "(unsaved changes)");
    }

    ImGui::End();
}
