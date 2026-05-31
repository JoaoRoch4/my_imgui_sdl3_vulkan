#pragma once

#include "pch.hpp"
class ImageViewerPanel;
class VideoPlayer;
class VideoPlayerPlacebo;
class BulkImageOpenQueue;
class MediaHistoryManager;
class VideoDownloader;
class OpenedFilesWindow;
class vulkan_context;

/**
 * Routes pending file paths and URLs to the correct media consumer.
 *
 * Extracted from MainMenuBar::Build() steps 2 and 3 so that all routing
 * decisions (video vs image, cached vs streaming, bulk vs single) live in
 * one place rather than inline in the menu-bar frame loop.
 *
 * All pointers are non-owning; call setup() before any process_* method.
 */
class MediaLoadHandler {
public:
    MediaLoadHandler();
    ~MediaLoadHandler() = default;

    MediaLoadHandler(const MediaLoadHandler &) = delete;
    MediaLoadHandler &operator=(const MediaLoadHandler &) = delete;

    void setup(ImageViewerPanel    *viewer,
               VideoPlayer         *video_player,
               VideoPlayerPlacebo  *video_player_placebo,
               BulkImageOpenQueue  *bulk_queue,
               MediaHistoryManager *history,
               VideoDownloader     *downloader,
               OpenedFilesWindow   *files_window,
               vulkan_context      *vk,
               bool                 use_video_player_placebo);

    void process_pending_paths(const std::vector<std::string>& paths);
    void process_pending_urls(const std::vector<std::string>& urls);
    void drain_bulk_queue();
    void restore_from_history();
    void set_use_video_player_placebo(bool enabled);

private:
    ImageViewerPanel   *m_viewer;       ///< Renders decoded image frames.
    VideoPlayer        *m_video_player; ///< Renders video streams.
    VideoPlayerPlacebo *m_video_player_placebo; ///< Optional VPP backend.
    BulkImageOpenQueue *m_bulk_queue;   ///< Off-thread path existence validator.
    MediaHistoryManager*m_history;      ///< Push / persist history entries.
    VideoDownloader    *m_downloader;   ///< Background video file cache.
    OpenedFilesWindow  *m_files_window; ///< Companion list panel for history sync.
    vulkan_context     *m_vk;           ///< Active Vulkan device for texture upload.
    bool                m_use_video_player_placebo; ///< Route calls through VPP.
};