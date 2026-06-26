#pragma once

#include "pch.hpp"

class vulkan_context;
class ManagedThread;

class VideoHoverPreview {
public:
    static inline ImVec2 preview_size = {1920, 1080};

    /// Runtime-mutable: enable/disable the hover preview popup entirely.
    static inline bool enabled = true;

    /// Runtime-mutable: play audio in hover preview (default: muted).
    static inline bool preview_sound = true;

    bool has_val =false;


    /// Last known native resolution of the loaded source (0×0 when unknown).
    static inline ImVec2 last_source_size = {0.0f, 0.0f};

    /// Runtime-mutable: dwell time before the popup appears and mpv starts loading.
    static inline std::chrono::milliseconds hover_delay{300};

    /// Kill the worker thread when no hover requests arrive for this interval.
    static constexpr std::chrono::milliseconds idle_thread_timeout{900};

    /// If preview loading stays stuck longer than this, restart the hover thread.
    /// MUST exceed worst-case first-frame latency. Internet sources resolve via
    /// yt-dlp and buffer over the network, which routinely takes several seconds;
    /// a short value here makes the watchdog kill the in-progress load every
    /// cooldown and reallocate the worker thread + mpv cache/demuxer buffers in a
    /// tight loop (the "internet video allocation loop"), so the source never
    /// finishes loading. Genuine *thread* hangs are recovered independently by
    /// ManagedThread's 5 s RestartOnTimeout overwatch — this is only the
    /// "source not progressing" backstop, so it can afford to be generous.
    static constexpr std::chrono::milliseconds loading_restart_timeout{950};

    /// Prevent rapid restart loops when a source is persistently broken.
    static constexpr std::chrono::milliseconds loading_restart_cooldown{900};

    /// If a hovered/playing source has no decoded frame for too long, force
    /// recovery. Same network-latency reasoning as loading_restart_timeout.
    static constexpr std::chrono::milliseconds no_frame_restart_timeout{950};

    VideoHoverPreview();
    ~VideoHoverPreview();

    void setup(vulkan_context *vk);
    void shutdown();

    VkDescriptorSet thumbnail(const std::string &source);
    void notify_hover(const std::string &source);
    bool save_frame(const std::filesystem::path &path);
    void tick_idle();

    /// True while a source is actively loaded/playing in the popup (so the caller
    /// can route arrow-key seeks here instead of to the main player).
    [[nodiscard]] bool is_previewing() const noexcept;
    /// Seek the preview's mpv by `seconds` (negative rewinds). No-op when idle.
    /// Safe to call from the UI thread while the worker polls events (mpv async API).
    void seek_relative(double seconds);

    /// Toggle pause on the currently-playing preview. No-op when idle.
    void toggle_pause();
    /// Current playback speed (1.0 = normal). Returns 1.0 when unknown/idle.
    [[nodiscard]] double speed() const;
    /// Set playback speed (used by the Space hold-to-fast-forward FSM).
    void set_speed(double s);
    /// Raise/lower preview volume by `delta` (clamped 0..130). Unmutes on raise
    /// and remembers the choice across source reloads without touching the
    /// persisted `preview_sound` config flag.
    void adjust_volume(int delta);

    bool consume_popup_reopen_request();
    [[nodiscard]] bool is_hover_dwell_pending(const std::string &source) const;

    struct GpuSlot {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        VkSampler sampler{};
        VkDescriptorSet descriptor{};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
        bool has_frame = false;
    };

    // core
    void init_mpv();
    void start_thread();
    void stop_thread();

    void load_source(const std::string &source);
    void start_playback(const std::string &source);
    void stop_playback();

    void flush_pending_upload();

    // slot (single texture, no LRU cache)
    bool create_slot();
    void destroy_slot();

    // vulkan
    bool create_shared();
    void destroy_shared();

private:
    vulkan_context *m_vk{};

    GpuSlot m_slot{};
    std::string m_slot_source;

    std::string m_current;
    std::string m_playing;
    std::chrono::steady_clock::time_point m_last_load_time{};
    std::chrono::steady_clock::time_point m_last_use_time{};
    std::chrono::steady_clock::time_point m_last_restart_time{};
    uint64_t m_watchdog_restart_count{0};
    uint64_t m_idle_stop_count{0};
    std::string m_last_restart_source;

    std::string m_hovered_source;
    std::chrono::steady_clock::time_point m_hover_start{};

    // Runtime audio state for keyboard volume control. m_user_unmuted overrides
    // the muted-by-default preview so a reload (new hover source) keeps the sound
    // the user dialled in, without writing back to the persisted preview_sound.
    int  m_volume{100};
    bool m_user_unmuted{false};

    mpv_handle *m_mpv{};
    mpv_render_context *m_render{};

    std::atomic<bool> m_frame_dirty{false};
    std::atomic<bool> m_waiting{false};
    std::atomic<bool> m_upload_pending{false};
    std::atomic<bool> m_popup_reopen_requested{false};

    std::unique_ptr<ManagedThread> m_thread;

    std::vector<uint8_t> m_buf;
    std::mutex m_buf_mutex;
    std::string m_pending_source;

    VkBuffer m_staging{};
    VkDeviceMemory m_staging_mem{};
    void *m_mapped{};

    VkCommandPool m_pool{};
    VkCommandBuffer m_cmd{};
    VkFence m_fence{};

    int m_w{}, m_h{};
};