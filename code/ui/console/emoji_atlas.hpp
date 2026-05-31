#pragma once

#include "pch.hpp"


// EmojiAtlas rasterizes a fixed set of emoji/color glyph codepoints from a
// FreeType-loadable font (COLRv1 or OT-SVG) and packs them into a single RGBA
// GPU texture atlas.  The caller obtains an ImTextureRef + per-glyph UV extents
// and is responsible for drawing via ImGui::Image() / ImDrawList::AddImage().
//
// Usage pattern (one-time, after the ImGui backend is initialized):
//
//   VulkanEmojiAtlas atlas(vulkan_ctx);
//   atlas.Build("/usr/share/fonts/noto/NotoColorEmoji.ttf", 32.0f, codepoints);
//
//   // Per-frame rendering:
//   if (auto* g = atlas.LookupGlyph(U'🚀')) {
//       ImGui::Image(atlas.GetTextureRef(), ImVec2(g->RenderW, g->RenderH),
//                   ImVec2(g->U0, g->V0), ImVec2(g->U1, g->V1));
//   }

class EmojiAtlas
{
public:
    // Per-glyph layout data (UV in [0,1], pixel bearing/advance at the build size).
    struct GlyphEntry
    {
        float U0, V0, U1, V1;  // UV coordinates in the atlas texture
        float AdvanceX;         // horizontal advance (pixels at GlyphSize)
        float BearingX;         // bitmap_left (pixels)
        float BearingY;         // bitmap_top (pixels, positive = above baseline)
        int   RenderW;          // rendered bitmap width (pixels)
        int   RenderH;          // rendered bitmap height (pixels)
    };

    EmojiAtlas();
    virtual ~EmojiAtlas() = default;

    // Rasterize all codepoints from font_path at size_px using FreeType (COLRv1 +
    // OT-SVG via PlutoSVG), pack into an atlas, call UploadRGBA(), then release
    // the CPU buffer.  Returns true on success.
    bool Build(const std::string& font_path, float size_px,
               const std::vector<ImWchar>& codepoints,
               const std::vector<std::string>& sequences = {});

    // Look up a glyph.  Returns nullptr for codepoints not in the atlas.
    [[nodiscard]] const GlyphEntry* LookupGlyph(ImWchar cp) const;

    // Look up a pre-shaped UTF-8 emoji sequence (e.g. family / ZWJ / skin-tone clusters).
    [[nodiscard]] const GlyphEntry* LookupSequence(std::string_view sequence) const;

    // Find the longest cached sequence beginning at byte offset in a UTF-8 string.
    [[nodiscard]] bool LookupSequencePrefix(std::string_view text, size_t offset,
                                            const GlyphEntry*& glyph,
                                            size_t& matched_bytes) const;

    [[nodiscard]] bool HasSequences() const { return !SequenceGlyphs_.empty(); }

    // ImTextureRef wrapping the uploaded GPU texture.  Valid after Build().
    [[nodiscard]] ImTextureRef GetTextureRef() const { return ImTextureRef{TextureID_}; }

    // Pixel size that was passed to Build().
    [[nodiscard]] float GlyphSize() const { return GlyphSize_; }

    // Atlas dimensions (0 before Build()).
    [[nodiscard]] int AtlasWidth()  const { return AtlasW_; }
    [[nodiscard]] int AtlasHeight() const { return AtlasH_; }

    // Debug: write the CPU-side atlas pixels to a PNG file (requires stb_image_write).
    // Valid after Build().  Returns false if no pixels are available or write fails.
    bool DumpAtlasToPng(const std::string& path) const;

protected:
    // Subclasses implement GPU upload.  RGBA data is row-major, 4 bytes/pixel.
    // Must return a valid ImTextureID (e.g. VkDescriptorSet cast for Vulkan)
    // or ImTextureID_Invalid on failure.
    virtual ImTextureID UploadRGBA(const std::vector<uint8_t>& pixels,
                                   int width, int height) = 0;

    ImTextureID TextureID_ { ImTextureID_Invalid };

private:
    float GlyphSize_ { 0.0f };
    int   AtlasW_    { 0 };
    int   AtlasH_    { 0 };

    std::unordered_map<ImWchar, GlyphEntry>  Glyphs_;
    std::unordered_map<std::string, GlyphEntry> SequenceGlyphs_;
    std::vector<std::string>                 SequenceKeys_;
    std::vector<uint8_t>                     AtlasPixels_; // retained CPU copy for DumpAtlasToPng
};
