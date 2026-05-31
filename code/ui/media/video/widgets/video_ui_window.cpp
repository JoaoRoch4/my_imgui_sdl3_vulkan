#include "video_ui_window.hpp"

#include "history_preview.hpp"
#include "recent_history_menu.hpp"
#include "video_context_menu.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

namespace {

    /**
 * @brief Reads the CPU model name from /proc/cpuinfo (first "model name" entry).
 *
 * Cached by the caller via a function-local static; this function is called
 * at most once per process.  Returns "Unknown CPU" if the file is absent or
 * the key is missing.
 *
 * @return Trimmed string, e.g. "Intel(R) Core(TM) i7-7700 CPU @ 3.60GHz".
 */
[[nodiscard]] static std::string read_cpu_name_linux()
{
    // /proc/cpuinfo is a virtual kernel file; open cost is negligible.
    std::ifstream f("/proc/cpuinfo");
    if (!f.is_open())
        return "Unknown CPU";                                                      // kernel file absent

    std::string line;
    while (std::getline(f, line)) {
        if (!line.starts_with("model name"))
            continue;                                                               // skip unrelated keys

        const std::size_t colon = line.find(':');                                  // separator position
        if (colon == std::string::npos)
            continue;                                                               // malformed line

        // Advance past ": " to reach the bare model string.
        std::string name = line.substr(colon + 2);

        // Strip trailing whitespace / carriage-return (CRLF files on some kernels).
        while (!name.empty() && (name.back() == ' ' || name.back() == '\r'))
            name.pop_back();

        return name;                                                               // first core entry is enough
    }
    return "Unknown CPU";                                                          // key not found in file
}

/**
 * @brief Reads the GPU model name from the NVIDIA sysfs tree or DRM fallback.
 *
 * Primary path: /proc/driver/nvidia/gpus/<PCI>/information — "Model:" line.
 * Fallback:     /sys/class/drm/cardN/device/product_name   — first non-empty.
 *
 * Cached by the caller; called at most once per process lifetime.
 *
 * @return String such as "NVIDIA GeForce RTX 3060", or "Unknown GPU".
 */
[[nodiscard]] static std::string read_gpu_name_linux()
{
    namespace fs = std::filesystem;
    std::error_code ec;

    // -- NVIDIA proprietary driver: /proc/driver/nvidia/gpus/<PCI>/information --
    const fs::path nvidia_base("/proc/driver/nvidia/gpus");
    if (fs::exists(nvidia_base, ec)) {
        for (const auto &gpu_dir : fs::directory_iterator(nvidia_base, ec)) {
            std::ifstream f(gpu_dir.path() / "information");
            if (!f.is_open())
                continue;                                                           // PCI dir without info file

            std::string line;
            while (std::getline(f, line)) {
                if (!line.starts_with("Model:"))
                    continue;

                const std::size_t colon = line.find(':');
                if (colon == std::string::npos)
                    continue;

                std::string name = line.substr(colon + 2);
                while (!name.empty() && (name.back() == ' ' || name.back() == '\r'))
                    name.pop_back();

                return name;                                                       // e.g. "NVIDIA GeForce RTX 3060"
            }
        }
    }

    // -- DRM fallback: works for Mesa / AMD / Intel open-source drivers ---------
    for (int card_idx = 0; card_idx < 4; ++card_idx) {
        // Probe up to card3; most desktops have at most two GPU nodes.
        const fs::path product_file =
            fs::path("/sys/class/drm") /
            ("card" + std::to_string(card_idx)) /
            "device" / "product_name";

        std::ifstream f(product_file);
        if (!f.is_open())
            continue;                                                               // card node absent

        std::string name;
        std::getline(f, name);                                                     // single-line file

        while (!name.empty() && (name.back() == ' '  ||
                                  name.back() == '\n' ||
                                  name.back() == '\r'))
            name.pop_back();

        if (!name.empty())
            return name;                                                           // first non-empty entry wins
    }

    return "Unknown GPU";                                                          // neither path produced a result
}

std::string format_time(double time_seconds)
{
    const int total_seconds = static_cast<int>(time_seconds);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", total_seconds / 60, total_seconds % 60);
    return std::string(buffer);
}

void draw_recent_menu(const VideoUiWindow::Callbacks &callbacks)
{
    if (!callbacks.history_provider || !callbacks.on_open_recent)
        return;

    const auto &history = callbacks.history_provider();
    if (history.empty() || !ImGui::BeginMenu("Recent"))
        return;

    RecentHistoryMenu::draw_entries(
        history,
        {
            .on_open = [&callbacks](WindowStateToml::ImageHistoryEntry &entry) {
                callbacks.on_open_recent(entry.source, entry.kind);
            },
            .on_hover = [&callbacks](WindowStateToml::ImageHistoryEntry &entry) {
                if (!callbacks.history_preview || !callbacks.lookup_history)
                    return;
                if (auto *found = callbacks.lookup_history(entry.source))
                    callbacks.history_preview->draw_for_hover(*found);
            },
            .on_after_item = nullptr,
        });

    ImGui::EndMenu();
}

void draw_open_shortcuts(const std::string &source,
                         const char        *startup_label,
                         const VideoUiWindow::Callbacks &callbacks)
{
    if (callbacks.on_open_image && ImGui::MenuItem("Open Image...", "Ctrl+O"))
        callbacks.on_open_image();
    if (callbacks.on_open_online && ImGui::MenuItem("Open Online..."))
        callbacks.on_open_online();
    if (callbacks.on_toggle_startup_video && ImGui::MenuItem(startup_label))
        callbacks.on_toggle_startup_video(source);

    draw_recent_menu(callbacks);
}

// ---------------------------------------------------------------------------
// CPU / GPU usage helpers
// ---------------------------------------------------------------------------

struct CpuTimes { unsigned long long idle = 0, total = 0; };

static CpuTimes read_cpu_times()
{
    std::ifstream f("/proc/stat");
    std::string tag;
    unsigned long long u, n, s, i, wa, irq, si, steal;
    f >> tag >> u >> n >> s >> i >> wa >> irq >> si >> steal;
    CpuTimes t;
    t.idle  = i + wa;
    t.total = u + n + s + i + wa + irq + si + steal;
    return t;
}

// Returns 0-100 CPU usage between two snapshots.
static int cpu_usage_pct(const CpuTimes &a, const CpuTimes &b)
{
    const unsigned long long dt = b.total - a.total;
    if (dt == 0) return 0;
    const unsigned long long di = b.idle  - a.idle;
    return static_cast<int>(100ULL * (dt - di) / dt);
}

struct GpuStats { int util_pct = -1; int64_t vram_used_mb = -1; int64_t vram_total_mb = -1; };

// Try NVML via dlopen; return false if unavailable.
static bool query_gpu_nvml(GpuStats &out)
{
    using nvmlReturn_t = int;
    using nvmlDevice_t = void *;
    struct nvmlUtilization_t { unsigned int gpu; unsigned int memory; };
    struct nvmlMemory_t { unsigned long long total; unsigned long long free; unsigned long long used; };

    static void *lib = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (!lib) lib = dlopen("libnvidia-ml.so", RTLD_LAZY | RTLD_LOCAL);
        if (lib) {
            auto init = reinterpret_cast<nvmlReturn_t(*)()>(dlsym(lib, "nvmlInit_v2"));
            if (!init) init = reinterpret_cast<nvmlReturn_t(*)()>(dlsym(lib, "nvmlInit"));
            if (init) init();
        }
    }
    if (!lib) return false;

    auto getHandle = reinterpret_cast<nvmlReturn_t(*)(unsigned int, nvmlDevice_t *)>(
        dlsym(lib, "nvmlDeviceGetHandleByIndex_v2"));
    auto getUtil   = reinterpret_cast<nvmlReturn_t(*)(nvmlDevice_t, nvmlUtilization_t *)>(
        dlsym(lib, "nvmlDeviceGetUtilizationRates"));
    auto getMem    = reinterpret_cast<nvmlReturn_t(*)(nvmlDevice_t, nvmlMemory_t *)>(
        dlsym(lib, "nvmlDeviceGetMemoryInfo"));
    if (!getHandle || !getUtil || !getMem) return false;

    nvmlDevice_t dev = nullptr;
    if (getHandle(0, &dev) != 0 || !dev) return false;

    nvmlUtilization_t util{};
    if (getUtil(dev, &util) == 0) out.util_pct = static_cast<int>(util.gpu);

    nvmlMemory_t mem{};
    if (getMem(dev, &mem) == 0) {
        out.vram_used_mb  = static_cast<int64_t>(mem.used  / (1024 * 1024));
        out.vram_total_mb = static_cast<int64_t>(mem.total / (1024 * 1024));
    }
    return out.util_pct >= 0;
}

// Fallback: read GPU utilisation from sysfs (amdgpu / radeon).
static bool query_gpu_sysfs(GpuStats &out)
{
    namespace fs = std::filesystem;
    static std::string s_util_path, s_vram_used_path, s_vram_total_path;
    static bool s_scanned = false;
    if (!s_scanned) {
        s_scanned = true;
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator("/sys/class/drm", ec)) {
            const auto name = entry.path().filename().string();
            if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos)
                continue;
            const auto base = entry.path() / "device";
            const auto u    = base / "gpu_busy_percent";
            if (fs::exists(u, ec)) {
                s_util_path        = u.string();
                s_vram_used_path   = (base / "mem_info_vram_used").string();
                s_vram_total_path  = (base / "mem_info_vram_total").string();
                break;
            }
        }
    }
    if (s_util_path.empty()) return false;

    auto read_int = [](const std::string &path, int64_t &val) {
        std::ifstream f(path);
        return static_cast<bool>(f >> val);
    };
    int64_t util = -1, vram_used = -1, vram_total = -1;
    if (!read_int(s_util_path, util)) return false;
    out.util_pct = static_cast<int>(util);
    read_int(s_vram_used_path,  vram_used);
    read_int(s_vram_total_path, vram_total);
    if (vram_used  >= 0) out.vram_used_mb  = vram_used  / (1024 * 1024);
    if (vram_total >= 0) out.vram_total_mb = vram_total / (1024 * 1024);
    return true;
}

static GpuStats query_gpu()
{
    GpuStats g;
    if (!query_gpu_nvml(g)) query_gpu_sysfs(g);
    return g;
}

struct StatsCache {
    std::chrono::steady_clock::time_point next_sample;
    std::array<std::string, 12> lines;
    CpuTimes last_cpu_times{};
    // Pre-computed every time lines are refreshed, reused every frame.
    float box_w = 0.0f;
    float box_h = 0.0f;
};

struct AutoHideState {
    std::chrono::steady_clock::time_point last_input;
};

void draw_stats_overlay(mpv_handle *mpv, ImVec2 image_pos, ImVec2 display_size)
{
    static std::unordered_map<mpv_handle *, StatsCache> s_cache;

    StatsCache &cache = s_cache[mpv];
    const auto now = std::chrono::steady_clock::now();
    if (now >= cache.next_sample) {
        cache.next_sample = now + std::chrono::milliseconds(500);

        int64_t video_bitrate = 0;
        int64_t audio_bitrate = 0;
        int64_t frame_drop    = 0;
        int64_t mistimed      = 0;
        int64_t width         = 0;
        int64_t height        = 0;
        double  fps           = 0.0;
        double  duration      = 0.0;
        double  file_size     = 0.0;
        mpv_get_property(mpv, "video-bitrate",        MPV_FORMAT_INT64,  &video_bitrate);
        mpv_get_property(mpv, "audio-bitrate",        MPV_FORMAT_INT64,  &audio_bitrate);
        mpv_get_property(mpv, "frame-drop-count",     MPV_FORMAT_INT64,  &frame_drop);
        mpv_get_property(mpv, "mistimed-frame-count", MPV_FORMAT_INT64,  &mistimed);
        mpv_get_property(mpv, "width",                MPV_FORMAT_INT64,  &width);
        mpv_get_property(mpv, "height",               MPV_FORMAT_INT64,  &height);
        mpv_get_property(mpv, "container-fps",        MPV_FORMAT_DOUBLE, &fps);
        mpv_get_property(mpv, "duration",             MPV_FORMAT_DOUBLE, &duration);
        mpv_get_property(mpv, "file-size",            MPV_FORMAT_DOUBLE, &file_size);

        char *video_codec   = nullptr;
        char *hwdec_current = nullptr;
        mpv_get_property(mpv, "video-codec",    MPV_FORMAT_STRING, &video_codec);
        mpv_get_property(mpv, "hwdec-current",  MPV_FORMAT_STRING, &hwdec_current);
        std::string decoder_str = "Decoder:  ";
        decoder_str += (video_codec && video_codec[0]) ? video_codec : "?";
        if (hwdec_current && hwdec_current[0]) {
            decoder_str += " (";
            decoder_str += hwdec_current;
            decoder_str += ')';
        }
        if (video_codec)   mpv_free(video_codec);
        if (hwdec_current) mpv_free(hwdec_current);

        // Format helpers
        auto format_bps = [](int64_t bps) -> std::string {
            std::array<char, 32> buf{};
            if (bps >= 1'000'000)
                std::snprintf(buf.data(), buf.size(), "%.1f Mbps",
                              static_cast<double>(bps) / 1'000'000.0);
            else
                std::snprintf(buf.data(), buf.size(), "%lld Kbps",
                              static_cast<long long>(bps / 1000));
            return std::string(buf.data());
        };
        auto format_duration = [](double secs) -> std::string {
            if (secs <= 0.0) return "?";
            const int h = static_cast<int>(secs) / 3600;
            const int m = (static_cast<int>(secs) % 3600) / 60;
            const int s = static_cast<int>(secs) % 60;
            std::array<char, 16> buf{};
            if (h > 0)
                std::snprintf(buf.data(), buf.size(), "%d:%02d:%02d", h, m, s);
            else
                std::snprintf(buf.data(), buf.size(), "%d:%02d", m, s);
            return std::string(buf.data());
        };
        auto format_size = [](double bytes) -> std::string {
            std::array<char, 32> buf{};
            if (bytes >= 1073741824.0)
                std::snprintf(buf.data(), buf.size(), "%.2f GiB", bytes / 1073741824.0);
            else if (bytes >= 1048576.0)
                std::snprintf(buf.data(), buf.size(), "%.1f MiB", bytes / 1048576.0);
            else
                std::snprintf(buf.data(), buf.size(), "%.0f KiB", bytes / 1024.0);
            return std::string(buf.data());
        };

        std::string res_str = "Resolution: ";
        if (width > 0 && height > 0)
            res_str += std::to_string(width) + "x" + std::to_string(height);
        else
            res_str += "?";

        std::string fps_str = "FPS:        ";
        if (fps > 0.0) {
            std::array<char, 16> buf{};
            std::snprintf(buf.data(), buf.size(), "%.2f", fps);
            fps_str += buf.data();
        } else { fps_str += "?"; }

        const int64_t total_bitrate = video_bitrate + audio_bitrate;

        // Lazy-initialised hardware strings — OS file I/O runs exactly once.
        // function-local statics are initialised on first call and then reused,
        // so the /proc and /sys reads never happen on subsequent 500 ms ticks.
        static const std::string s_cpu_name = read_cpu_name_linux();              // e.g. "Intel(R) Core(TM) i7-7700 @ 3.60GHz"
        static const std::string s_gpu_name = read_gpu_name_linux();              // e.g. "NVIDIA GeForce RTX 3060"

        cache.lines = {
            std::move(res_str),                                                    // [0] Resolution: 1920x1080
            std::move(fps_str),                                                    // [1] FPS:        60.00
            "Duration: " + format_duration(duration),                             // [2] Duration: 5:45
            "Size:     " + (file_size > 0.0 ? format_size(file_size)              //
                                             : std::string("?")),                 // [3] Size:     165.6 MiB
            "Total BR: " + format_bps(total_bitrate),                             // [4] Total BR: 4.1 Mbps
            "Video:    " + format_bps(video_bitrate),                             // [5] Video:    3.7 Mbps
            "Audio:    " + format_bps(audio_bitrate),                             // [6] Audio:    334 Kbps
            "Dropped:  " + std::to_string(frame_drop),                            // [7] Dropped:  6
            "Mistimed: " + std::to_string(mistimed),                              // [8] Mistimed: 0
            std::move(decoder_str),                                                // [9] Decoder:  H.264 / AVC (nvdec)
            "CPU:      " + s_cpu_name,                                            // [10] CPU: Intel(R) Core(TM) i7-7700 @ 3.60GHz
            "GPU:      " + s_gpu_name,                                            // [11] GPU: NVIDIA GeForce RTX 3060
        };

        // Recompute box dimensions only when content changes (every 500 ms),
        // not every frame.
        const float pad_x    = 10.0f;
        const float pad_y    = 8.0f;
        const float line_gap = 3.0f;
        const float line_h   = ImGui::GetTextLineHeight();

        float max_w = 0.0f;
        for (const auto &l : cache.lines)
            max_w = std::max(max_w, ImGui::CalcTextSize(l.c_str()).x);

        const int n   = static_cast<int>(cache.lines.size());
        cache.box_w   = max_w + pad_x * 2.0f;
        cache.box_h   = static_cast<float>(n) * (line_h + line_gap) - line_gap + pad_y * 2.0f;
    }

    if (cache.box_w <= 0.0f)
        return;

    const float pad_x    = 10.0f;
    const float pad_y    = 8.0f;
    const float line_gap = 3.0f;
    const float line_h   = ImGui::GetTextLineHeight();

    const ImVec2 box_min = {image_pos.x + 8.0f, image_pos.y + 8.0f};
    const ImVec2 box_max = {box_min.x + cache.box_w, box_min.y + cache.box_h};

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(box_min, box_max,
                      ImGui::GetColorU32(ImVec4(0.04f, 0.04f, 0.04f, 0.82f)), 6.0f);
    dl->AddRect(box_min, box_max,
                ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.18f)), 6.0f);

    const int n = static_cast<int>(cache.lines.size());
    for (int i = 0; i < n; ++i) {
        dl->AddText(
            ImVec2(box_min.x + pad_x,
                   box_min.y + pad_y + static_cast<float>(i) * (line_h + line_gap)),
            ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)),
            cache.lines[static_cast<size_t>(i)].c_str());
    }
}

} // namespace

void VideoUiWindow::draw(State state, const Callbacks &callbacks) const
{
    const auto is_fullscreen_active = [&state, &callbacks]() -> bool {
        if (callbacks.on_get_app_fullscreen)
            return callbacks.on_get_app_fullscreen();
        return state.fullscreen;
    };

    const auto set_fullscreen_active = [&state, &callbacks](bool enabled) {
        if (callbacks.on_set_app_fullscreen)
            callbacks.on_set_app_fullscreen(enabled);
        else
            state.fullscreen = enabled;
    };

    const bool startup_fixed = callbacks.is_startup_video_fixed &&
                               callbacks.is_startup_video_fixed(state.source);
    const char *startup_label = startup_fixed ? "Unfix Startup Videos" : "Set Startup Videos";

    const auto toggle_pause = [&state, &callbacks]() {
        int paused = 0;
        mpv_get_property(state.mpv, "pause", MPV_FORMAT_FLAG, &paused);
        int next_paused = paused ? 0 : 1;
        if (callbacks.on_set_playback_state)
            callbacks.on_set_playback_state(state.id, next_paused == 0);
        else
            mpv_set_property(state.mpv, "pause", MPV_FORMAT_FLAG, &next_paused);
        state.osd.show(next_paused ? "Paused" : "Playing");
    };

    const auto seek_by = [&state](int delta_seconds) {
        const std::string command = "seek " + std::to_string(delta_seconds);
        mpv_command_string(state.mpv, command.c_str());
        state.osd.show(delta_seconds >= 0
                           ? "Seek +" + std::to_string(delta_seconds) + "s"
                           : "Seek " + std::to_string(delta_seconds) + "s");
    };

    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (is_fullscreen_active()) {
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
        flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus;
    } else {
        ImGui::SetNextWindowSize(ImVec2(640.0f, 420.0f), ImGuiCond_FirstUseEver);
        if (callbacks.on_open_image || callbacks.on_open_online || callbacks.on_open_recent)
            flags |= ImGuiWindowFlags_MenuBar;
    }

    const std::string window_id = state.display_title + "###video_" + std::to_string(state.id);
    if (!ImGui::Begin(window_id.c_str(), &state.open, flags)) {
        ImGui::End();
        return;
    }

    if (callbacks.context_menu) {
        WindowStateToml::ImageHistoryEntry history_entry;
        if (callbacks.lookup_history) {
            if (const auto *found = callbacks.lookup_history(state.source))
                history_entry = *found;
        }
        if (history_entry.source.empty()) {
            history_entry.source = state.source;
            history_entry.kind = state.kind;
        }

        const std::string popup_id = "##vctx_" + std::to_string(state.id);
        if (ImGui::BeginPopupContextWindow(popup_id.c_str())) {
            const auto context_result = callbacks.context_menu->draw_menu_items(history_entry);
            if (context_result.erase && callbacks.on_erase_history)
                callbacks.on_erase_history(context_result.erase_source);
            if (context_result.restart_preview && callbacks.on_restart_hover_preview)
                callbacks.on_restart_hover_preview();

            ImGui::Separator();
            if (ImGui::MenuItem("Reload Video")) {
                state.reload_requested = true;
                state.osd.show("Reloading...");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Show Status", nullptr, state.show_stats))
                state.show_stats = !state.show_stats;
            const bool fullscreen_active = is_fullscreen_active();
            if (ImGui::MenuItem("Fullscreen", nullptr, fullscreen_active)) {
                const bool next_fullscreen = !fullscreen_active;
                set_fullscreen_active(next_fullscreen);
                state.osd.show(next_fullscreen ? "Fullscreen" : "Windowed");
            }
            if (ImGui::MenuItem("Hide UI", nullptr, state.hide_ui))
                state.hide_ui = !state.hide_ui;
            if (ImGui::MenuItem("Auto-hide UI", nullptr, state.auto_hide_ui))
                state.auto_hide_ui = !state.auto_hide_ui;
            if (ImGui::MenuItem("Close Video")) {
                state.open = false;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Close All Videos")) {
                if (callbacks.on_close_all_videos)
                    callbacks.on_close_all_videos();
                ImGui::CloseCurrentPopup();
            }

            if (callbacks.on_open_image || callbacks.on_open_online || callbacks.on_open_recent) {
                ImGui::Separator();
                draw_open_shortcuts(state.source, startup_label, callbacks);
            }
            ImGui::EndPopup();
        }
    }

    if (!is_fullscreen_active() && ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            draw_open_shortcuts(state.source, startup_label, callbacks);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    if (is_fullscreen_active() && ImGui::IsKeyPressed(ImGuiKey_Escape))
        set_fullscreen_active(false);

    constexpr float k_controls_height = 72.0f;
    constexpr auto k_auto_hide_timeout = std::chrono::milliseconds(1500);
    const ImVec2 available = ImGui::GetContentRegionAvail();

    static std::unordered_map<int, AutoHideState> s_auto_hide;
    const bool window_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                                        ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    const auto now = std::chrono::steady_clock::now();
    AutoHideState &auto_hide = s_auto_hide[state.id];
    if (auto_hide.last_input.time_since_epoch().count() == 0)
        auto_hide.last_input = now;

    const ImGuiIO &io = ImGui::GetIO();
    const bool mouse_moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    const bool mouse_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                               ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                               ImGui::IsMouseClicked(ImGuiMouseButton_Middle);

    if (window_hovered && (mouse_moved || mouse_clicked))
        auto_hide.last_input = now;

    const bool auto_hidden_now = state.auto_hide_ui &&
                                 (!window_hovered || (now - auto_hide.last_input) >= k_auto_hide_timeout);
    const bool show_controls = !state.hide_ui && !auto_hidden_now;

    if (state.auto_hide_ui && auto_hidden_now && window_hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);

    const float effective_controls_height = show_controls ? k_controls_height : 0.0f;

    if (state.descriptor_set != VK_NULL_HANDLE && state.video_w > 0 && state.video_h > 0) {
        const float video_aspect = static_cast<float>(state.video_w) / static_cast<float>(state.video_h);
        const float canvas_height = std::max(available.y - effective_controls_height, 1.0f);
        const float canvas_width = available.x;

        float display_width = 0.0f;
        float display_height = 0.0f;
        if (video_aspect > canvas_width / canvas_height) {
            display_width = canvas_width;
            display_height = canvas_width / video_aspect;
        } else {
            display_height = canvas_height;
            display_width = canvas_height * video_aspect;
        }

        display_width = std::max(display_width, 1.0f);
        display_height = std::max(display_height, 1.0f);

        const ImVec2 cursor_base = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(cursor_base.x + (canvas_width - display_width) * 0.5f,
                                   cursor_base.y + (canvas_height - display_height) * 0.5f));
        const ImVec2 image_pos = ImGui::GetCursorScreenPos();
        ImGui::Image(std::bit_cast<ImTextureID>(state.descriptor_set),
                     ImVec2(display_width, display_height));
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                const bool next_fullscreen = !is_fullscreen_active();
                set_fullscreen_active(next_fullscreen);
                state.osd.show(next_fullscreen ? "Fullscreen" : "Windowed");
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                toggle_pause();
            }
        }

        if (is_fullscreen_active() && state.downloaded_bytes > 0 && !state.osd.visible()) {
            constexpr double k_mb = 1024.0 * 1024.0;
            char progress_buf[64];
            std::snprintf(progress_buf,
                          sizeof(progress_buf),
                          "Downloading %.1f MB",
                          static_cast<double>(state.downloaded_bytes) / k_mb);
            state.osd.show(progress_buf, std::chrono::milliseconds(700));
        }

        state.osd.draw(image_pos, ImVec2(display_width, display_height));
        if (state.show_stats)
            draw_stats_overlay(state.mpv, image_pos, ImVec2(display_width, display_height));
    } else if (state.load_failed) {
        ImGui::TextDisabled("Failed to load");
    } else {
        ImGui::TextDisabled("Loading...");
    }

    if (!show_controls) {
        ImGui::End();
        return;
    }

    ImGui::Separator();

    double time_pos = 0.0;
    double duration = 0.0;
    int paused = 0;
    int64_t volume = 100;
    mpv_get_property(state.mpv, "time-pos", MPV_FORMAT_DOUBLE, &time_pos);
    mpv_get_property(state.mpv, "duration", MPV_FORMAT_DOUBLE, &duration);
    mpv_get_property(state.mpv, "pause", MPV_FORMAT_FLAG, &paused);
    mpv_get_property(state.mpv, "volume", MPV_FORMAT_INT64, &volume);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 1.0f));

    if (!state.has_prev)
        ImGui::BeginDisabled();
    if (ImGui::SmallButton("|<") && callbacks.on_switch_relative)
        callbacks.on_switch_relative(-1);
    if (!state.has_prev)
        ImGui::EndDisabled();

    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::SmallButton("<<"))
        seek_by(seek_seconds_button_backward);

    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::SmallButton(paused ? "|>" : "||"))
        toggle_pause();

    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::SmallButton(">>"))
        seek_by(static_cast<int>(seek_seconds_button_foward));

    ImGui::SameLine(0.0f, 4.0f);
    const bool loop_style_pushed = state.loop;
    if (loop_style_pushed)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::SmallButton("⟳")) {
        state.loop = !state.loop;
        mpv_command_string(state.mpv, state.loop ? "set loop-file inf" : "set loop-file no");
        state.osd.show(state.loop ? "Loop On" : "Loop Off");
    }
    if (loop_style_pushed)
        ImGui::PopStyleColor();

    ImGui::SameLine(0.0f, 4.0f);
    if (!state.has_next)
        ImGui::BeginDisabled();
    if (ImGui::SmallButton(">|") && callbacks.on_switch_relative)
        callbacks.on_switch_relative(1);
    if (!state.has_next)
        ImGui::EndDisabled();

    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::SmallButton("Reload")) {
        state.reload_requested = true;
        state.osd.show("Reloading...");
    }

    ImGui::PopStyleVar();

    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextUnformatted(format_time(time_pos).c_str());
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::TextDisabled("/");
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::TextUnformatted(format_time(duration).c_str());

    if (duration > 0.0) {
        float pos_fraction = static_cast<float>(time_pos / duration);
        ImGui::SetNextItemWidth(-140.0f);
        if (ImGui::SliderFloat("##seek", &pos_fraction, 0.0f, 1.0f, "")) {
            double new_pos = pos_fraction * duration;
            mpv_set_property(state.mpv, "time-pos", MPV_FORMAT_DOUBLE, &new_pos);
        }

        if (ImGui::IsItemHovered()) {
            if (callbacks.on_seek_preview_hover)
                callbacks.on_seek_preview_hover(state.id);

            // Lazily start the seek-preview mpv instance + thread on first hover.
            state.seek_preview.ensure_active();

            if (state.seek_preview.descriptor_set() != VK_NULL_HANDLE) {
                const ImVec2 item_min = ImGui::GetItemRectMin();
                const ImVec2 item_max = ImGui::GetItemRectMax();
                const float fraction = std::clamp(
                    (ImGui::GetMousePos().x - item_min.x) / (item_max.x - item_min.x),
                    0.0f,
                    1.0f);
                state.seek_preview.seek(static_cast<double>(fraction) * duration);
                ImGui::BeginTooltip();
                ImGui::Image(std::bit_cast<ImTextureID>(state.seek_preview.descriptor_set()),
                             state.seek_preview.size());
                ImGui::Text("%s", format_time(static_cast<double>(fraction) * duration).c_str());
                ImGui::EndTooltip();
            }
        }

        ImGui::SameLine();
    }

    int volume_percent = static_cast<int>(volume);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderInt("##vol", &volume_percent, 0, 150, "Vol %d%%")) {
        int64_t new_volume = volume_percent;
        mpv_set_property(state.mpv, "volume", MPV_FORMAT_INT64, &new_volume);
        state.osd.show("Volume " + std::to_string(volume_percent) + "%");
    }

    ImGui::End();
}