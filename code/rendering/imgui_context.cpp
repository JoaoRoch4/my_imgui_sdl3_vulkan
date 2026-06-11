#include "pch.hpp"

#include "imgui_context.hpp"

#include "debug_log.hpp"

#include <fontconfig/fontconfig.h>

namespace {

// Case-insensitive substring test (ASCII). Used to confirm fontconfig actually
// matched the family we asked for -- FcFontMatch always returns a *nearest* match,
// so without this guard a request for an uninstalled family silently yields some
// unrelated default font.
[[nodiscard]] bool contains_ci(std::string_view haystack, std::string_view needle) noexcept {
    const auto eq = [](char a, char b) noexcept {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    return !std::ranges::search(haystack, needle, eq).empty();
}

// Resolve the absolute on-disk path of the best fontconfig match for `family`,
// but only when the matched font's own family name contains `family` -- otherwise
// return "" so the caller can fall through to the next candidate. This makes font
// discovery portable across distros (Debian/Fedora/Arch lay fonts out differently)
// instead of hard-coding /usr/share/fonts/... paths that break per-system.
[[nodiscard]] std::string resolve_font_by_family(std::string_view family) {
    if (FcInit() == FcFalse) {
        return {};
    }

    const std::string family_z{family};
    const std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> pattern{
        FcPatternCreate(), &FcPatternDestroy};
    if (!pattern) {
        return {};
    }
    FcPatternAddString(pattern.get(), FC_FAMILY,
                       reinterpret_cast<const FcChar8 *>(family_z.c_str()));
    FcConfigSubstitute(nullptr, pattern.get(), FcMatchPattern);
    FcDefaultSubstitute(pattern.get());

    FcResult result{};
    const std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> matched{
        FcFontMatch(nullptr, pattern.get(), &result), &FcPatternDestroy};
    if (!matched || result != FcResultMatch) {
        return {};
    }

    FcChar8 *matched_family = nullptr;
    if (FcPatternGetString(matched.get(), FC_FAMILY, 0, &matched_family) != FcResultMatch
        || matched_family == nullptr
        || !contains_ci(reinterpret_cast<const char *>(matched_family), family)) {
        return {};
    }

    FcChar8 *file = nullptr;
    if (FcPatternGetString(matched.get(), FC_FILE, 0, &file) != FcResultMatch || file == nullptr) {
        return {};
    }
    return std::string{reinterpret_cast<const char *>(file)};
}

// True when this process is being traced (TracerPid != 0 in /proc/self/status) -- i.e. running
// under a debugger. Re-read on every call (calls are rare) so attaching later is still picked up.
[[nodiscard]] bool is_debugger_present() noexcept {
    constexpr std::string_view tracer_key{"TracerPid:"};

    std::ifstream status{"/proc/self/status"};
    if (!status.is_open()) {
        APP_DEBUG_LOG("[imgui_context] is_debugger_present: cannot open /proc/self/status");
        return false;
    }

    for (std::string line; std::getline(status, line);) {
        std::string_view view{line};
        if (!view.starts_with(tracer_key)) {
            continue;
        }

        // Trim the key and surrounding whitespace using bounds-checked string_view
        // index operations -- no pointer arithmetic, no fixed-size buffers.
        view.remove_prefix(tracer_key.size());
        const std::size_t first = view.find_first_not_of(" \t");
        const std::size_t last = view.find_last_not_of(" \t");
        if (first == std::string_view::npos) {
            break;
        }
        view = view.substr(first, last - first + 1);

        const bool attached = view != "0";
        APP_DEBUG_LOG("[imgui_context] is_debugger_present: TracerPid='{}' attached={}", view, attached);
        return attached;
    }

    APP_DEBUG_LOG("[imgui_context] is_debugger_present: TracerPid line not found");
    return false;
}

} // namespace

imgui_context::imgui_context()
    : font_cousine(nullptr)
    , font_droid_sans(nullptr)
    , font_karla(nullptr)
    , font_proggy_clean(nullptr)
    , font_proggy_tiny(nullptr)
    , font_roboto(nullptr)
{}
void imgui_context::init(SDL_Window* window, vulkan_context& vk, ImGui_ImplVulkanH_Window* wd, float main_scale)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Only surface ImGui's debugger-break tools (the Metrics "**DebugBreak**"
    // buttons) when a debugger is actually attached. The Item Picker itself is
    // always available, but its IM_DEBUG_BREAK() is made inert when undebugged
    // (see imconfig.h: ImAppIsDebuggerAttached), so it can't crash a normal run.
    io.ConfigDebugIsDebuggerPresent = is_debugger_present();

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplSDL3_InitForVulkan(window);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance       = vk.instance;
    init_info.PhysicalDevice = vk.physical_device;
    init_info.Device         = vk.device;
    init_info.QueueFamily    = vk.queue_family;
    init_info.Queue          = vk.queue;
    init_info.PipelineCache  = vk.pipeline_cache;
    init_info.DescriptorPool = vk.descriptor_pool;
    init_info.MinImageCount  = vk.min_image_count;
    init_info.ImageCount     = wd->ImageCount;
    init_info.Allocator      = vk.allocator;
    init_info.PipelineInfoMain.RenderPass   = wd->RenderPass;
    init_info.PipelineInfoMain.Subpass      = 0;
    init_info.PipelineInfoMain.MSAASamples  = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = vulkan_context::check_result;
    ImGui_ImplVulkan_Init(&init_info);

    load_fonts(main_scale);
}

void imgui_context::load_fonts(float main_scale)
{
    const float base_size = 16.0f * main_scale;
    ImFontAtlas *atlas = ImGui::GetIO().Fonts;

    // ── Resolve the multilingual fallback chain ONCE ──────────────────────────
    // For any codepoint a base font lacks, ImGui 1.92's dynamic loader walks the
    // merged sources in order and uses the first that actually has the glyph
    // (imgui_draw.cpp: ImFontBaked_BuildLoadGlyph). GlyphRanges are NOT consulted on
    // that path -- ORDER is what matters: the base font is always source[0] so ASCII
    // stays crisp, then each fallback supplies only what the earlier ones miss.
    // Ordered most-specific → broadest so mono symbol fonts win over CJK's copies of
    // the same arrows/shapes, while SMP emoji (absent from symbol fonts) reach the
    // color emoji face.
    struct fallback_spec {
        std::initializer_list<std::string_view> families; // resolved via fontconfig, in order
        std::initializer_list<std::string_view> literals;  // last-resort absolute paths
        bool color;                                        // load color layers (emoji)
    };

    const std::array<fallback_spec, 8> specs{{
        {{"Noto Sans Symbols 2"},
         {"/usr/share/fonts/google-noto/NotoSansSymbols2-Regular.ttf",
          "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf"},
         false},
        {{"Noto Sans Symbols"}, {}, false},
        {{"Noto Sans Math"}, {}, false},
        {{"Noto Color Emoji"},
         {"/usr/share/fonts/google-noto-color-emoji-fonts/Noto-COLRv1.ttf",
          "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf"},
         true},
        {{"Noto Sans CJK JP", "Noto Sans CJK SC"}, {}, false},
        {{"Noto Sans Hebrew"}, {}, false},
        {{"Noto Naskh Arabic", "Noto Sans Arabic"}, {}, false},
        {{"Noto Sans Egyptian Hieroglyphs"}, {}, false},
    }};

    struct loaded_fallback {
        std::span<std::byte> bytes; // points into fallback_font_blobs_ (stable, owns data)
        bool color;
    };
    std::vector<loaded_fallback> chain;
    chain.reserve(specs.size());

    fallback_font_blobs_.clear();
    fallback_font_blobs_.reserve(specs.size()); // no realloc → spans below stay valid

    for (const fallback_spec &spec : specs) {
        std::string path;
        for (std::string_view family : spec.families) {
            path = resolve_font_by_family(family);
            if (!path.empty()) {
                break;
            }
        }
        if (path.empty()) {
            for (std::string_view literal : spec.literals) {
                std::error_code ec;
                if (std::filesystem::exists(literal, ec)) {
                    path.assign(literal);
                    break;
                }
            }
        }
        if (path.empty()) {
            const std::string_view first = spec.families.size() ? *spec.families.begin()
                                                                : std::string_view{"?"};
            std::println(stderr, "[imgui_context] no font found for fallback '{}'", first);
            continue;
        }

        std::ifstream in{path, std::ios::binary | std::ios::ate};
        if (!in) {
            std::println(stderr, "[imgui_context] cannot open fallback font '{}'", path);
            continue;
        }
        const std::streamsize size = in.tellg();
        in.seekg(0);
        std::vector<std::byte> blob(static_cast<std::size_t>(size));
        if (!in.read(reinterpret_cast<char *>(blob.data()), size)) {
            std::println(stderr, "[imgui_context] cannot read fallback font '{}'", path);
            continue;
        }
        fallback_font_blobs_.push_back(std::move(blob));
        chain.push_back({std::span<std::byte>{fallback_font_blobs_.back()}, spec.color});
        std::println("[imgui_context] fallback font: {}{}", path, spec.color ? "  (color)" : "");
    }

    // Merge the whole resolved chain into one base font. The bytes are shared (not
    // owned by the atlas), so the same large face is referenced -- never copied -- by
    // every base font.
    auto merge_chain = [&](float size_px) {
        for (const loaded_fallback &fb : chain) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.FontDataOwnedByAtlas = false; // bytes live in fallback_font_blobs_
            cfg.SizePixels = size_px;
            cfg.PixelSnapH = !fb.color;
            if (fb.color) {
                cfg.FontLoaderFlags |=
                    ImGuiFreeTypeLoaderFlags_LoadColor | ImGuiFreeTypeLoaderFlags_Bitmap;
            }
            atlas->AddFontFromMemoryTTF(static_cast<void *>(fb.bytes.data()),
                                        static_cast<int>(fb.bytes.size()), size_px, &cfg);
        }
    };

    auto load = [&](std::string_view filename) -> ImFont * {
        const std::string path = std::format("{}/{}", IMGUI_FONTS_DIR, filename);
        ImFont *f = atlas->AddFontFromFileTTF(path.c_str(), base_size);
        IM_ASSERT(f != nullptr);
        merge_chain(base_size);
        return f;
    };

    font_cousine      = load("Cousine-Regular.ttf");
    font_droid_sans   = load("DroidSans.ttf");
    font_karla        = load("Karla-Regular.ttf");
    font_proggy_clean = load("ProggyClean.ttf");
    font_proggy_tiny  = load("ProggyTiny.ttf");
    font_roboto       = load("Roboto-Medium.ttf");
}

void imgui_context::shutdown()
{
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void imgui_context::new_frame()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void imgui_context::render(ImGui_ImplVulkanH_Window* wd, vulkan_context& vk, const ImVec4& clear_color)
{
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
    if (!is_minimized)
    {
        vk.frame_render(wd, draw_data, clear_color);
        vk.frame_present(wd);
    }
}
