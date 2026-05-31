#pragma once

#include "pch.hpp"
#include "placebo_egl_context.hpp"
#include "video_osd_overlay.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <mpv/render_gl.h>

#include <libplacebo/vulkan.h>
#include <libplacebo/renderer.h>

// ---------------------------------------------------------------------------
// PlaceboEntry
//
// Owns all per-video-window state for the NVDEC → no-copy → Vulkan path:
//
//   NVDEC (CUDA)
//     ↓  (CUDA-GL interop inside mpv / vo-libmpv + hwdec=nvdec)
//   OpenGL FBO  ←→  Vulkan image  (shared via GL_EXT_memory_object_fd /
//     ↑                             VK_KHR_external_memory_fd)
//   mpv renders here
//     ↓  (VkSemaphore ↔ GL semaphore  via GL_EXT_semaphore_fd)
//   pl_tex (libplacebo wraps the shared VkImage)
//     ↓  (pl_renderer: color space, tone mapping, upscale)
//   Output VkImage → VkDescriptorSet → ImGui::Image()
// ---------------------------------------------------------------------------
struct PlaceboEntry {
    // ---- mpv state ---------------------------------------------------------
    mpv_handle         *mpv        = nullptr;
    mpv_render_context *render_ctx = nullptr;
    std::atomic<bool>   frame_dirty{false};

    // ---- EGL context (one per entry, headless/surfaceless) -----------------
    PlaceboEglContext egl;

    // ---- Shared Vulkan ↔ GL image (mpv FBO target) -------------------------
    // This image is allocated with VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
    // so it can be imported into OpenGL via GL_EXT_memory_object_fd.
    VkImage        shared_image  = VK_NULL_HANDLE;
    VkDeviceMemory shared_memory = VK_NULL_HANDLE;
    VkFormat       shared_format = VK_FORMAT_R8G8B8A8_UNORM;
    int            shared_w      = 0;
    int            shared_h      = 0;

    // OpenGL objects that back the mpv FBO
    GLuint gl_mem_obj  = 0; // GL memory object (imported from VkDeviceMemory fd)
    GLuint gl_texture  = 0; // GL texture backed by gl_mem_obj
    GLuint gl_fbo      = 0; // framebuffer object mpv renders into

    // ---- Synchronisation semaphores (Vulkan ↔ GL) -------------------------
    // gl_render_done_sem: GL signals when mpv has finished rendering the frame.
    // vk_can_read_sem:    Vulkan waits on it before sampling the shared image.
    VkSemaphore vk_ready_sem  = VK_NULL_HANDLE; // Vulkan waits (GL signals)
    VkSemaphore vk_release_sem= VK_NULL_HANDLE; // GL waits (Vulkan signals after read)
    GLuint gl_ready_sem   = 0;
    GLuint gl_release_sem = 0;

    // Whether a rendered frame is waiting to be consumed by Vulkan/libplacebo
    bool frame_ready_for_pl = false;

    // ---- libplacebo textures -----------------------------------------------
    // pl_input_tex wraps shared_image so libplacebo can read it.
    // pl_output_tex is an RGBA image libplacebo writes the final frame into.
    pl_tex pl_input_tex  = nullptr;
    pl_tex pl_output_tex = nullptr;

    // ---- Output Vulkan image (extracted from pl_output_tex) ---------------
    // This is the VkImage ImGui samples via descriptor_set.
    VkDescriptorSet descriptor_set     = VK_NULL_HANDLE;
    VkSampler       output_sampler     = VK_NULL_HANDLE;
    VkImageView     output_image_view  = VK_NULL_HANDLE; // view of pl_output_tex's VkImage

    // pl_output_tex hold state (held = user controls it, SHADER_READ_ONLY layout)
    bool            pl_output_held     = false;
    VkSemaphore     output_hold_sem    = VK_NULL_HANDLE; // binary sem: libplacebo signals when hold done

    // Interop command resources – used to CPU-wait on the output hold semaphore
    VkCommandPool   interop_cmd_pool   = VK_NULL_HANDLE;
    VkCommandBuffer interop_cmd_buf    = VK_NULL_HANDLE;
    VkFence         interop_fence      = VK_NULL_HANDLE;

    // GL render bootstrapping flag
    bool            first_gl_render    = true;

    // ---- Entry metadata ----------------------------------------------------
    std::string source;
    std::string title;
    std::string playback_source;
    std::string kind;
    int  id             = 0;
    bool open           = true;
    bool load_failed    = false;
    bool hwdec_enabled  = true;
    int  resume_position_seconds = 0;
    bool resume_seek_pending     = false;

    VideoOsdOverlay osd;
};
