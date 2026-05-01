#pragma once

#include "vulkan_context.hpp"
#include "vulkan_texture.hpp"
#include "window_state_toml.hpp"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

class VideoPlayer;
class ImageViewerPanel;

class HistoryPreview {
public:
    HistoryPreview();
    ~HistoryPreview();

    HistoryPreview(const HistoryPreview &) = delete;
    HistoryPreview &operator=(const HistoryPreview &) = delete;

    void setup(vulkan_context *vk, VideoPlayer *vp = nullptr, ImageViewerPanel *viewer = nullptr);
    void shutdown();

    void set_thumb_dir(const std::filesystem::path &dir);
    void draw_for_hover(WindowStateToml::ImageHistoryEntry &hentry);

private:
    struct Job {
        std::string source;
        std::string kind;
    };

    struct JobResult {
        std::string source;
        std::filesystem::path path;
        bool is_temp_file;
        bool ok;
    };

    void request_preview(const WindowStateToml::ImageHistoryEntry &hentry);
    void clear_active_preview();
    void apply_ready_result(const WindowStateToml::ImageHistoryEntry &hentry);
    void worker_loop(std::stop_token stoken);

    vulkan_context *m_vk;
    VideoPlayer *m_video_player;
    ImageViewerPanel *m_viewer;

    std::jthread m_worker;
    std::mutex m_mutex;
    std::condition_variable m_cv;

    bool m_has_pending_job;
    Job m_pending_job;

    bool m_has_ready_result;
    JobResult m_ready_result;

    VulkanTexture m_active_texture;
    std::string m_active_source;
    std::filesystem::path m_active_temp_path;
    std::filesystem::path m_thumb_dir;
};