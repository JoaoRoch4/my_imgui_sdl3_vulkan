#pragma once

#include "pch.hpp"
#include "placebo_entry.hpp"
#include "window_state_toml.hpp"

#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

class vulkan_context;
class VideoPlayer;
class VideoContextMenu;
class HistoryPreview;
class VideoDownloader;

class VideoPlayerPlacebo {
public:
  struct Config {
    bool enable_hwdec;
    bool prefer_nvdec;

    [[nodiscard]] bool operator==(const Config &other) const noexcept {
      return enable_hwdec == other.enable_hwdec &&
             prefer_nvdec == other.prefer_nvdec;
    }

    [[nodiscard]] bool operator!=(const Config &other) const noexcept {
      return !(*this == other);
    }
  };

  static constexpr Config kDefaultConfig{true, true};

  VideoPlayerPlacebo();
  ~VideoPlayerPlacebo();

  VideoPlayerPlacebo(const VideoPlayerPlacebo &) = delete;
  VideoPlayerPlacebo &operator=(const VideoPlayerPlacebo &) = delete;

  void bind_context(vulkan_context *vk);

  void setup(vulkan_context *vk);
  void setup(vulkan_context *vk, Config config);
  void reconfigure(Config config);
  void shutdown();

  bool add_from_path(const std::filesystem::path &path);
  bool add_from_path(const std::filesystem::path &path,
                     const std::string &title);
  bool add_from_path(const std::filesystem::path &path,
                     const std::string &title,
                     const std::string &logical_source);
  bool add_from_path(const std::filesystem::path &path,
                     const std::string &title,
                     const std::string &logical_source, bool hwdec_enabled,
                     int resume_position_seconds,
                     const std::string &initial_osd_message);

  bool add_from_url(const std::string &url, const std::string &title);
  bool add_from_url(const std::string &url, const std::string &title,
                    bool hwdec_enabled, int resume_position_seconds,
                    const std::string &initial_osd_message);

  void update_frames();
  void draw();

  void notify_download_complete(const std::string &url,
                                const std::filesystem::path &cached_path);
  void replace_source_with_saved_file(const std::string &source,
                                      const std::filesystem::path &saved_path);

  bool close_window(const std::string &source);
  void close_all_windows();
  [[nodiscard]] bool has_open_windows() const;
  [[nodiscard]] std::vector<std::string> open_sources() const;
  [[nodiscard]] VkDescriptorSet
  get_open_thumbnail(const std::string &source) const;
  [[nodiscard]] VkDescriptorSet hover_thumbnail(const std::string &source);
  void notify_hover(const std::string &source);
  bool save_hover_frame(const std::filesystem::path &path);

  static bool is_video_path(const std::filesystem::path &path);
  static bool is_video_url(const std::string &url);

  void set_downloader(VideoDownloader *d);
  void restart_hover_preview();
  [[nodiscard]] bool consume_hover_popup_reopen_request();
  [[nodiscard]] bool is_hover_dwell_pending(const std::string &source) const;

  [[nodiscard]] bool can_toggle_hwdec(const std::string &source) const;
  [[nodiscard]] bool is_hwdec_enabled(const std::string &source) const;
  [[nodiscard]] int current_position_seconds(const std::string &source) const;
  [[nodiscard]] int persisted_position_seconds(const std::string &source) const;
  void set_resume_persist_min_duration_seconds(int seconds);
  [[nodiscard]] int resume_persist_min_duration_seconds() const;
  void toggle_hwdec(const std::string &source);
  void sync_history_state(
      std::vector<WindowStateToml::ImageHistoryEntry> &history) const;

  void set_all_hwdec(bool enabled);
  void set_all_loop(bool enabled);
  void restart_all_threads();

  void set_context_menu(
      VideoContextMenu *ctx,
      std::function<WindowStateToml::ImageHistoryEntry *(const std::string &)>
          lookup,
      std::function<void(const std::string &)> on_erase);

  void set_player_menu_callbacks(
      std::function<void()> on_open_image, std::function<void()> on_open_online,
      std::function<void(const std::string &, const std::string &)>
          on_open_recent,
      std::function<const std::vector<WindowStateToml::ImageHistoryEntry> &()>
          history,
      HistoryPreview *preview = nullptr,
      std::function<void(const std::string &)> on_fix_videos = nullptr,
      std::function<bool(const std::string &)> is_startup_videos_fixed =
          nullptr,
      std::function<bool()> on_get_app_fullscreen = nullptr,
      std::function<void(bool)> on_set_app_fullscreen = nullptr);

  static constexpr ImVec2 k_preview_size{320.0f, 180.0f};

  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] const Config &config() const noexcept;
  void reset_to_defaults();

private:
  // ---- Vulkan / libplacebo GPU state (shared across all entries) ----------
  bool init_placebo_gpu();
  void destroy_placebo_gpu();

  // ---- Per-entry lifecycle -----------------------------------------------
  bool entry_create_shared_image(PlaceboEntry &e, int w, int h);
  void entry_destroy_shared_image(PlaceboEntry &e);
  bool entry_create_sync(PlaceboEntry &e);
  void entry_destroy_sync(PlaceboEntry &e);
  bool entry_create_mpv(PlaceboEntry &e, const std::string &path, bool hwdec);
  void entry_destroy_mpv(PlaceboEntry &e);
  void entry_poll_events(PlaceboEntry &e);
  void entry_render_frame(PlaceboEntry &e); // mpv → libplacebo → output image
  void entry_destroy_all(PlaceboEntry &e);
  void entry_destroy_output_descriptor(PlaceboEntry &e);

  // ---- Placeholder texture (shown before first frame) --------------------
  bool create_placeholder_texture();
  void destroy_placeholder_texture();

  // ---- Members -----------------------------------------------------------
  vulkan_context *m_vk = nullptr;
  Config m_config{kDefaultConfig};
  bool m_initialized = false;
  int m_next_id = 0;
  int m_resume_persist_min_duration_seconds =
      WindowStateToml{}.video_resume_persist_min_duration_seconds;
  bool m_global_loop_enabled = false;

  // libplacebo GPU objects (imported from m_vk)
  pl_log m_pl_log = nullptr;
  pl_vulkan m_pl_vk = nullptr; // imported from m_vk — does NOT own device
  pl_renderer m_pl_renderer = nullptr;

  // Per-video entries
  std::vector<std::unique_ptr<PlaceboEntry>> m_entries;

  // Delegate player for hover/seek preview (uses SW path, not placebo)
  std::unique_ptr<VideoPlayer> m_hover_player;
  VideoDownloader *m_downloader = nullptr;

  // Context menu / callbacks
  VideoContextMenu *m_ctx_menu = nullptr;
  std::function<WindowStateToml::ImageHistoryEntry *(const std::string &)>
      m_ctx_lookup;
  std::function<void(const std::string &)> m_ctx_on_erase;

  std::function<void()> m_on_open_image;
  std::function<void()> m_on_open_online;
  std::function<void(const std::string &, const std::string &)>
      m_on_open_recent;
  std::function<const std::vector<WindowStateToml::ImageHistoryEntry> &()>
      m_history_provider;
  HistoryPreview *m_history_preview = nullptr;
  std::function<void(const std::string &)> m_on_fix_videos;
  std::function<bool(const std::string &)> m_is_startup_videos_fixed;
  std::function<bool()> m_on_get_app_fullscreen;
  std::function<void(bool)> m_on_set_app_fullscreen;

  // Main-thread guard (mirrors VideoPlayer)
  std::thread::id m_main_thread_id;

  // Placeholder texture (checkerboard, shown while video loads)
  int m_placeholder_width = 256;
  int m_placeholder_height = 144;
  VkDescriptorSet m_placeholder_descriptor_set = VK_NULL_HANDLE;
  VkSampler m_placeholder_sampler = VK_NULL_HANDLE;
  VkImageView m_placeholder_image_view = VK_NULL_HANDLE;
  VkImage m_placeholder_image = VK_NULL_HANDLE;
  VkDeviceMemory m_placeholder_image_memory = VK_NULL_HANDLE;
};
