#include "VideoEntry.hpp"
#include "pch.hpp"

/**
 * @brief Default-constructs a VideoEntry with every Vulkan handle null and every flag false.
 *
 * All VkHandle fields start as VK_NULL_HANDLE so that destroy_gpu_resources() can safely
 * test each one before calling the matching vkDestroy* function without reading
 * uninitialised memory.
 *
 * StagingSlot carries its own default member initialisers (VK_NULL_HANDLE / nullptr / false),
 * so value-initialising @c staging_slots{} via the member-initialiser list is sufficient —
 * there is no need to list the individual slot fields here.
 *
 * @c container_fps defaults to 30.0 and @c frame_interval to 33 ms as a conservative
 * fallback that is active until VIDEO_RECONFIG fires and provides the real container
 * frame rate.  The throttle tightens automatically once the rate is known.
 */
VideoEntry::VideoEntry()
    : mpv{nullptr}                                      // no mpv handle until add_from_path/url
    , render_ctx{nullptr}                               // no render context until mpv is initialised
    , frame_dirty{false}                                // nothing to upload yet
    , video_w{0}                                        // dimensions unknown until VIDEO_RECONFIG
    , video_h{0}
    , container_fps{30.0}                               // safe fallback frame rate
    , frame_interval{std::chrono::milliseconds(33)}     // 1/30 fps ≈ 33 ms until fps is known
    , image{VK_NULL_HANDLE}                             // device-local texture; created on reconfig
    , image_memory{VK_NULL_HANDLE}                      // backing memory for image
    , image_view{VK_NULL_HANDLE}                        // full-subresource view
    , sampler{VK_NULL_HANDLE}                           // linear clamp-to-border sampler
    , descriptor_set{VK_NULL_HANDLE}                    // ImGui texture descriptor
    , staging_slots{}                                   // StagingSlot default-inits all fields to null
    , staging_write_idx{0}                              // begin by writing to slot 0
    , cmd_pool{VK_NULL_HANDLE}                          // shared command pool for both staging slots
    , last_upload_time{}                                // epoch; first upload always passes the gate
    , title{}
    , source{}
    , playback_source{}
    , kind{}
    , id{0}
    , open{true}                                        // newly created entries are visible
    , fullscreen{false}
    , loop{false}
    , hwdec_enabled{false}
    , resume_position_seconds{0}
    , resume_seek_pending{false}
    , reload_requested{false}
    , load_failed{false}
    , media_unloaded{false}
    , intentional_stop_pending{false}
    , finished_at_eof{false}
    , show_stats{false}
    , hide_ui{false}
    , auto_hide_ui{false}
    , reload_osd_message{}
    , osd{}
{
}