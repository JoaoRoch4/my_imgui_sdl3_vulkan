#pragma once

#include "pch.hpp"
#include "video_osd_overlay.hpp"
#include "video_seek_preview.hpp"
#include "window_state_toml.hpp"

struct VideoEntry {
    VideoEntry();

    mpv_handle         *mpv;         ///< libmpv player handle; null when destroyed.
    mpv_render_context *render_ctx;  ///< libmpv software render context.
    std::atomic<bool>   frame_dirty; ///< Set by the mpv render callback; cleared by upload_frame.

    int video_w; ///< Decoded video width in pixels;  0 until VIDEO_RECONFIG fires.
    int video_h; ///< Decoded video height in pixels; 0 until VIDEO_RECONFIG fires.

    double                                   container_fps;   ///< Frame rate from container metadata; defaults to 30.
    std::chrono::steady_clock::duration      frame_interval;  ///< Upload throttle = 90% of one frame period.

    // Vulkan device-local texture (created once on VIDEO_RECONFIG)
    VkImage         image;          ///< Device-local 2-D RGBA texture sampled by ImGui.
    VkDeviceMemory  image_memory;   ///< Backing device allocation for image.
    VkImageView     image_view;     ///< Full-subresource view; registered with the sampler.
    VkSampler       sampler;        ///< Linear sampler; registered with ImGui_ImplVulkan.
    VkDescriptorSet descriptor_set; ///< ImGui_ImplVulkan descriptor for the texture.

    // -------------------------------------------------------------------------
    // Double-buffered staging pipeline.
    //
    // Two slots alternate so the GPU can finish reading slot N while slot N+1
    // is already being filled by mpv.  This eliminates the single-buffer stall
    // that was the primary cause of the chronic frame drops.
    // -------------------------------------------------------------------------

    /// One staging slot: a host-visible buffer + command buffer + fence.
    struct StagingSlot {
        VkBuffer        buf       = VK_NULL_HANDLE; ///< Host-visible transfer-source buffer.
        VkDeviceMemory  mem       = VK_NULL_HANDLE; ///< Persistently-mapped backing memory.
        void           *mapped    = nullptr;         ///< CPU pointer into mem; mpv renders directly here.
        VkCommandBuffer cmd       = VK_NULL_HANDLE; ///< Upload command buffer for this slot.
        VkFence         fence     = VK_NULL_HANDLE; ///< Signaled by the GPU when the upload finishes.
        bool            in_flight = false;           ///< True while the GPU is reading this slot.
    };

    static constexpr std::size_t k_staging_count = 2; ///< Number of ping-pong staging slots.

    std::array<StagingSlot, k_staging_count> staging_slots;   ///< Ping-pong staging resources.
    std::size_t                              staging_write_idx; ///< Next slot index to render into.

    VkCommandPool cmd_pool; ///< Shared resettable pool; both staging slots allocate from it.

    std::chrono::steady_clock::time_point last_upload_time; ///< Time of the most recent successful upload.

    // Entry metadata
    std::string title;           ///< Display name shown in the window title bar.
    std::string source;          ///< Logical source key (path or URL) used for history lookup.
    std::string playback_source; ///< Actual path or URL passed to mpv loadfile.
    std::string kind;            ///< "video", "gif", or "audio".
    int         id;              ///< Unique monotone ID assigned at construction.
    bool        open;            ///< False triggers teardown on the next draw() call.
    bool        fullscreen;      ///< Whether the window is in fullscreen mode.
    bool        loop;            ///< Whether loop-file is set in mpv.
    bool        hwdec_enabled;   ///< Whether NVDEC hardware decode is active.
    int         resume_position_seconds;  ///< Saved seek position to restore after reload.
    bool        resume_seek_pending;      ///< True while a restore-seek has not yet fired.
    bool        reload_requested;         ///< Request to close and re-open this entry.
    bool        load_failed;              ///< mpv reported a non-EOF end-of-file reason.
    bool        media_unloaded;           ///< True after mpv "stop"; keeps the last texture frozen.
    bool        intentional_stop_pending; ///< Suppresses the load_failed path for explicit stops.
    bool        finished_at_eof;          ///< True after natural EOF; resets resume position to 0.
    bool        show_stats;               ///< Whether the debug stats overlay is visible.
    bool        hide_ui;                  ///< Whether the control bar is hidden.
    bool        auto_hide_ui;             ///< Whether the control bar auto-hides on inactivity.
    std::string reload_osd_message;       ///< OSD text to show when the entry reloads.

    VideoOsdOverlay  osd;          ///< Timed on-screen display overlay.
    VideoSeekPreview seek_preview; ///< Dedicated mpv + jthread seek thumbnail generator.
};