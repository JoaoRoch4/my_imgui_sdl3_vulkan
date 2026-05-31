#include "pch.hpp"


#include "emoji_atlas.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H   // FT_Property_Set
#include FT_COLOR_H

#ifdef IMGUI_ENABLE_FREETYPE_PLUTOSVG
#include <plutosvg.h>
#endif


// STB_IMAGE_IMPLEMENTATION and STB_IMAGE_WRITE_IMPLEMENTATION are already
// defined in vulkan_texture.cpp; include the headers as declaration-only here.
#include <stb_image.h>
#include <stb_image_write.h>

// ── helpers ───────────────────────────────────────────────────────────────────

// Next power-of-two >= v.
static int NextPow2(int v)
{
    int r = 1;
    while (r < v)
        r <<= 1;
    return r;
}

static std::string ShellSingleQuote(std::string_view value)
{
    std::string quoted;
    quoted.reserve(value.size() + 8);
    quoted.push_back('\'');
    for (const char ch : value) {
        if (ch == '\'')
            quoted += "'\\''";
        else
            quoted.push_back(ch);
    }
    quoted.push_back('\'');
    return quoted;
}

// Convert FreeType BGRA pre-multiplied → RGBA straight-alpha in-place.
static void BgraPreMulToRgba(uint8_t* dst, const uint8_t* src, int w, int h,
    int src_pitch)
{
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + static_cast<ptrdiff_t>(y) * src_pitch;
        for (int x = 0; x < w; ++x) {
            uint8_t b = row[static_cast<ptrdiff_t>(x) * 4 + 0];
            uint8_t g = row[static_cast<ptrdiff_t>(x) * 4 + 1];
            uint8_t r = row[static_cast<ptrdiff_t>(x) * 4 + 2];
            uint8_t a = row[static_cast<ptrdiff_t>(x) * 4 + 3];
            if (a != 0 && a != 255) {
                r = static_cast<uint8_t>(
                    std::min(255, static_cast<int>(r) * 255 / static_cast<int>(a)));
                g = static_cast<uint8_t>(
                    std::min(255, static_cast<int>(g) * 255 / static_cast<int>(a)));
                b = static_cast<uint8_t>(
                    std::min(255, static_cast<int>(b) * 255 / static_cast<int>(a)));
            }
            // Re-order B-G-R-A → R-G-B-A
            uint8_t* out = dst + static_cast<ptrdiff_t>(y * w + x) * 4;
            out[0] = r;
            out[1] = g;
            out[2] = b;
            out[3] = a;
        }
    }
}

// Copy a grayscale FreeType bitmap as white-on-transparent RGBA.
static void GrayToRgba(uint8_t* dst, const uint8_t* src, int w, int h,
    int src_pitch)
{
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + static_cast<ptrdiff_t>(y) * src_pitch;
        for (int x = 0; x < w; ++x) {
            uint8_t v = row[x];
            uint8_t* out = dst + static_cast<ptrdiff_t>(y * w + x) * 4;
            out[0] = 255;
            out[1] = 255;
            out[2] = 255;
            out[3] = v;
        }
    }
}

// ── EmojiAtlas ────────────────────────────────────────────────────────────────

EmojiAtlas::EmojiAtlas() = default;

bool EmojiAtlas::Build(const std::string& font_path, float size_px,
    const std::vector<ImWchar>& codepoints,
    const std::vector<std::string>& sequences)
{
    Glyphs_.clear();
    SequenceGlyphs_.clear();
    SequenceKeys_.clear();
    AtlasPixels_.clear();
    TextureID_ = ImTextureID_Invalid;
    GlyphSize_ = size_px;
    AtlasW_ = 0;
    AtlasH_ = 0;

    if (codepoints.empty() && sequences.empty())
        return false;

    // ── FreeType init ────────────────────────────────────────────────────────
    FT_Library lib { nullptr };
    if (FT_Init_FreeType(&lib) != 0) {
        std::println(stderr, "[EmojiAtlas] FT_Init_FreeType failed");
        return false;
    }

    // Register PlutoSVG hooks so OT-SVG glyphs render via plutosvg.
#ifdef IMGUI_ENABLE_FREETYPE_PLUTOSVG
    FT_Property_Set(lib, "ot-svg", "svg-hooks", plutosvg_ft_svg_hooks());
#endif

    FT_Face face { nullptr };
    if (FT_New_Face(lib, font_path.c_str(), 0, &face) != 0) {
        std::println(stderr, "[EmojiAtlas] FT_New_Face failed for '{}'", font_path);
        FT_Done_Library(lib);
        return false;
    }

    if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) {
        std::println(stderr, "[EmojiAtlas] FT_Select_Charmap(UNICODE) failed for '{}'",
            font_path);
        FT_Done_Face(face);
        FT_Done_Library(lib);
        return false;
    }

    const int size_i = static_cast<int>(size_px);
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(size_i));

    // ── First pass: rasterize all glyphs, record metrics ─────────────────────
    // Each glyph gets a fixed cell of size_i × size_i in the atlas.
    const int cell = size_i;

    // Gather glyphs that actually exist in the font.
    struct RasterEntry {
        bool         is_sequence { false };
        ImWchar      cp { 0 };
        std::string  sequence;
        int          bm_w { 0 }, bm_h { 0 };
        int          bearing_x { 0 }, bearing_y { 0 };
        float        advance_x { 0.0f };
        std::vector<uint8_t> rgba; // size bm_w * bm_h * 4
    };
    std::vector<RasterEntry> entries;
    entries.reserve(codepoints.size() + sequences.size());
    const std::string quoted_font_path = ShellSingleQuote(font_path);

    auto load_with_hb_view = [&](ImWchar cp) {
        const std::filesystem::path tmp_path =
            std::filesystem::temp_directory_path() /
            std::format("emoji_atlas_{:08X}.png", static_cast<unsigned int>(cp));
        const std::string command = std::format(
            "hb-view {} --unicodes={:X} --output-format=png --output-file={} "
            "--margin=0 --background=00000000 --font-size={}",
            quoted_font_path,
            static_cast<unsigned int>(cp),
            ShellSingleQuote(tmp_path.string()),
            size_i);
        if (std::system(command.c_str()) != 0)
            return;

        int w = 0, h = 0, channels = 0;
        stbi_uc* rgba = stbi_load(tmp_path.c_str(), &w, &h, &channels, 4);
        if (rgba == nullptr || w <= 0 || h <= 0) {
            std::filesystem::remove(tmp_path);
            return;
        }

        RasterEntry e;
        e.cp = cp;
        e.bm_w = w;
        e.bm_h = h;
        e.bearing_x = 0;
        e.bearing_y = h;
        e.advance_x = static_cast<float>(w);
        e.rgba.assign(rgba, rgba + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
        stbi_image_free(rgba);
        std::filesystem::remove(tmp_path);
        entries.push_back(std::move(e));
    };

    auto load_sequence_with_hb_view = [&](const std::string& sequence) {
        const std::filesystem::path tmp_path =
            std::filesystem::temp_directory_path() /
            std::format("emoji_sequence_{:08X}.png",
                static_cast<unsigned int>(std::hash<std::string>{}(sequence)));
        const std::string command = std::format(
            "hb-view {} --text={} --output-format=png --output-file={} "
            "--margin=0 --background=00000000 --font-size={}",
            quoted_font_path,
            ShellSingleQuote(sequence),
            ShellSingleQuote(tmp_path.string()),
            size_i);
        if (std::system(command.c_str()) != 0)
            return;

        int w = 0, h = 0, channels = 0;
        stbi_uc* rgba = stbi_load(tmp_path.c_str(), &w, &h, &channels, 4);
        if (rgba == nullptr || w <= 0 || h <= 0) {
            std::filesystem::remove(tmp_path);
            return;
        }

        RasterEntry e;
        e.is_sequence = true;
        e.sequence = sequence;
        e.bm_w = w;
        e.bm_h = h;
        e.bearing_x = 0;
        e.bearing_y = h;
        e.advance_x = static_cast<float>(w);
        e.rgba.assign(rgba, rgba + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
        stbi_image_free(rgba);
        std::filesystem::remove(tmp_path);
        entries.push_back(std::move(e));
    };

    for (ImWchar cp : codepoints) {
        FT_UInt gidx = FT_Get_Char_Index(face, static_cast<FT_ULong>(cp));
        if (gidx == 0)
            continue; // not in font

        // Match imgui_freetype.cpp: load first, then render explicitly.
        if (FT_Load_Glyph(face, gidx, FT_LOAD_COLOR) != 0)
            continue;

        if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0)
            continue;

        FT_GlyphSlot slot = face->glyph;
        const FT_Bitmap& bm = slot->bitmap;
        const int w = static_cast<int>(bm.width);
        const int h = static_cast<int>(bm.rows);

        RasterEntry e;
        e.cp = cp;
        e.bm_w = w;
        e.bm_h = h;
        e.bearing_x = slot->bitmap_left;
        e.bearing_y = slot->bitmap_top;
        e.advance_x = static_cast<float>(slot->advance.x) / 64.0f;

        if (w > 0 && h > 0) {
            e.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0u);
            if (bm.pixel_mode == FT_PIXEL_MODE_BGRA) {
                BgraPreMulToRgba(e.rgba.data(), bm.buffer, w, h, bm.pitch);
            } else {
                // Fallback: grayscale → white glyphs
                GrayToRgba(e.rgba.data(), bm.buffer, w, h, bm.pitch);
            }
        }
        entries.push_back(std::move(e));
    }

    const bool has_visible_freetype_pixels = std::any_of(entries.begin(), entries.end(),
        [](const RasterEntry& entry) {
            if (entry.is_sequence)
                return false;
            return entry.bm_w > 0 && entry.bm_h > 0 && !entry.rgba.empty();
        });
    if (!has_visible_freetype_pixels) {
        entries.clear();
        for (ImWchar cp : codepoints)
            load_with_hb_view(cp);
    }

    for (const std::string& sequence : sequences)
        load_sequence_with_hb_view(sequence);

    FT_Done_Face(face);
    FT_Done_Library(lib);

    if (entries.empty()) {
        std::println(stderr, "[EmojiAtlas] No glyphs rasterized from '{}'", font_path);
        return false;
    }

    // ── Atlas packing: simple grid, each cell = cell × cell ──────────────────
    const int n    = static_cast<int>(entries.size());
    const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(n))));
    const int rows = (n + cols - 1) / cols;
    const int atlas_w = NextPow2(cols * cell);
    const int atlas_h = NextPow2(rows * cell);

    AtlasW_ = atlas_w;
    AtlasH_ = atlas_h;

    std::vector<uint8_t> pixels(
        static_cast<size_t>(atlas_w) * static_cast<size_t>(atlas_h) * 4u, 0u);

    for (int i = 0; i < n; ++i) {
        const RasterEntry& e = entries.at(static_cast<size_t>(i));
        const int col = i % cols;
        const int row = i / cols;
        const int ox  = col * cell;
        const int oy  = row * cell;

        // Blit glyph into the atlas (top-left aligned inside the cell).
        for (int y = 0; y < e.bm_h; ++y) {
            for (int x = 0; x < e.bm_w; ++x) {
                if ((ox + x) >= atlas_w || (oy + y) >= atlas_h)
                    continue;
                const size_t src_off =
                    (static_cast<size_t>(y) * static_cast<size_t>(e.bm_w) +
                     static_cast<size_t>(x)) * 4u;
                const size_t dst_off =
                    (static_cast<size_t>(oy + y) * static_cast<size_t>(atlas_w) +
                     static_cast<size_t>(ox + x)) * 4u;
                pixels.at(dst_off + 0) = e.rgba.at(src_off + 0);
                pixels.at(dst_off + 1) = e.rgba.at(src_off + 1);
                pixels.at(dst_off + 2) = e.rgba.at(src_off + 2);
                pixels.at(dst_off + 3) = e.rgba.at(src_off + 3);
            }
        }

        // Record UV extents for this glyph.
        GlyphEntry ge {};
        ge.U0       = static_cast<float>(ox)          / static_cast<float>(atlas_w);
        ge.V0       = static_cast<float>(oy)          / static_cast<float>(atlas_h);
        ge.U1       = static_cast<float>(ox + e.bm_w) / static_cast<float>(atlas_w);
        ge.V1       = static_cast<float>(oy + e.bm_h) / static_cast<float>(atlas_h);
        ge.AdvanceX = e.advance_x;
        ge.BearingX = static_cast<float>(e.bearing_x);
        ge.BearingY = static_cast<float>(e.bearing_y);
        ge.RenderW  = e.bm_w;
        ge.RenderH  = e.bm_h;

        if (e.is_sequence)
            SequenceGlyphs_.emplace(e.sequence, ge);
        else
            Glyphs_.emplace(e.cp, ge);
    }

    SequenceKeys_.reserve(SequenceGlyphs_.size());
    for (const auto& entry : SequenceGlyphs_)
        SequenceKeys_.push_back(entry.first);
    std::ranges::sort(SequenceKeys_, [](const std::string& lhs, const std::string& rhs) {
        return lhs.size() > rhs.size();
    });

    // ── GPU upload (backend-specific) ────────────────────────────────────────
    TextureID_ = UploadRGBA(pixels, atlas_w, atlas_h);
    if (TextureID_ == ImTextureID_Invalid) {
        std::println(stderr, "[EmojiAtlas] UploadRGBA failed");
        return false;
    }

    AtlasPixels_ = pixels;

    std::println("[EmojiAtlas] Built {}x{} atlas, {} glyphs, size={:.0f}px",
        atlas_w, atlas_h,
        static_cast<int>(Glyphs_.size() + SequenceGlyphs_.size()), size_px);
    return true;
}

const EmojiAtlas::GlyphEntry* EmojiAtlas::LookupGlyph(ImWchar cp) const
{
    auto it = Glyphs_.find(cp);
    return (it != Glyphs_.end()) ? &it->second : nullptr;
}

const EmojiAtlas::GlyphEntry* EmojiAtlas::LookupSequence(std::string_view sequence) const
{
    auto it = SequenceGlyphs_.find(std::string(sequence));
    return (it != SequenceGlyphs_.end()) ? &it->second : nullptr;
}

bool EmojiAtlas::LookupSequencePrefix(std::string_view text, size_t offset,
    const GlyphEntry*& glyph, size_t& matched_bytes) const
{
    glyph = nullptr;
    matched_bytes = 0;
    if (offset >= text.size())
        return false;

    const std::string_view tail = text.substr(offset);
    for (const std::string& key : SequenceKeys_) {
        if (!tail.starts_with(key))
            continue;
        auto it = SequenceGlyphs_.find(key);
        if (it == SequenceGlyphs_.end())
            continue;
        glyph = &it->second;
        matched_bytes = key.size();
        return true;
    }
    return false;
}

bool EmojiAtlas::DumpAtlasToPng(const std::string& path) const
{
    if (path.empty() || AtlasPixels_.empty() || AtlasW_ <= 0 || AtlasH_ <= 0)
        return false;

    const int result = stbi_write_png(path.c_str(), AtlasW_, AtlasH_, 4,
        AtlasPixels_.data(), AtlasW_ * 4);
    if (result == 0) {
        std::println(stderr, "[EmojiAtlas] Failed to write PNG '{}'", path);
        return false;
    }

    std::println("[EmojiAtlas] Wrote atlas PNG '{}'", path);
    return true;
}
