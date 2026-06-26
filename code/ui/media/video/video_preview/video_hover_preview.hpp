#pragma once

#include "pch.hpp"

class vulkan_context;
class ManagedThread;

class VideoHoverPreview {
	public:

		/// On-screen MAX size of the hover popup (display bound, user-configurable).
		/// The actual window is fit to the media's aspect ratio within this box and
		/// further clamped to a fraction of the screen, so it never overflows.
		/// NOTE: display-only — it does NOT size the GPU capture buffer, so changing
		/// it at runtime never reallocates Vulkan resources.
		static inline ImVec2 preview_size = {960, 540};

		/// FIXED resolution of the mpv SW render target / Vulkan texture / saved
		/// thumbnail. Decoupled from preview_size so the popup can be resized to the
		/// media without ever reallocating Vulkan resources (which would race the
		/// render worker / crash the device). 16:9 → zero letterbox for common video.
		static constexpr ImVec2 capture_size = {1920, 1080};

		/// Runtime-mutable: enable/disable the hover preview popup entirely.
		static inline bool enabled = true;

		/// Runtime-mutable: play audio in hover preview (default: muted).
		static inline bool preview_sound = false;

		/// Last known native (display) resolution of the loaded source, published by
		/// the mpv worker thread on VIDEO_RECONFIG and read on the UI thread. Stored as
		/// one packed 64-bit atomic (w:hi32, h:lo32) so the (w,h) pair is always read
		/// coherently — no torn reads, no mutex. Use the accessors below, not the field.
		static inline std::atomic<uint64_t> s_source_size_bits {0};

		static void set_source_size(int w, int h) noexcept {
			s_source_size_bits.store(
				(static_cast<uint64_t>(static_cast<uint32_t>(w)) << 32) |
				 static_cast<uint64_t>(static_cast<uint32_t>(h)),
				std::memory_order_relaxed);
		}
		[[nodiscard]] static ImVec2 source_size() noexcept {
			const uint64_t b = s_source_size_bits.load(std::memory_order_relaxed);
			return {static_cast<float>(b >> 32),
			        static_cast<float>(b & 0xFFFFFFFFu)};
		}

		/// Normalized sub-rectangle of the capture buffer actually covered by the
		/// (letterboxed, keepaspect=yes) video, for the given source + capture dims.
		/// Pure + constexpr so the UI can crop the bars on display and the saver can
		/// crop them out of the PNG — and so it is unit-checked at compile time.
		struct UvRect { float u0, v0, u1, v1; };
		static constexpr UvRect video_subrect(float src_w, float src_h,
		                                       float cap_w, float cap_h) noexcept {
			if (src_w <= 0.0f || src_h <= 0.0f || cap_w <= 0.0f || cap_h <= 0.0f)
				return {0.0f, 0.0f, 1.0f, 1.0f};
			const float src_aspect = src_w / src_h;
			const float cap_aspect = cap_w / cap_h;
			if (src_aspect > cap_aspect) {        // wider than buffer → bars top/bottom
				const float pad = (1.0f - cap_aspect / src_aspect) * 0.5f;
				return {0.0f, pad, 1.0f, 1.0f - pad};
			}
			const float pad = (1.0f - src_aspect / cap_aspect) * 0.5f;  // bars left/right
			return {pad, 0.0f, 1.0f - pad, 1.0f};
		}

		/// Runtime-mutable: dwell time before the popup appears and mpv starts loading.
		static inline std::chrono::milliseconds hover_delay {300};

		/// Kill the worker thread when no hover requests arrive for this interval.
		static constexpr std::chrono::milliseconds idle_thread_timeout {100};

		/// If preview loading stays stuck longer than this, restart the hover thread.
		/// MUST exceed worst-case first-frame latency. Internet sources resolve via
		/// yt-dlp and buffer over the network, which routinely takes several seconds;
		/// a short value here makes the watchdog kill the in-progress load every
		/// cooldown and reallocate the worker thread + mpv cache/demuxer buffers in a
		/// tight loop (the "internet video allocation loop"), so the source never
		/// finishes loading. Genuine *thread* hangs are recovered independently by
		/// ManagedThread's 5 s RestartOnTimeout overwatch — this is only the
		/// "source not progressing" backstop, so it can afford to be generous.
		static constexpr std::chrono::milliseconds loading_restart_timeout {1500};

		/// Prevent rapid restart loops when a source is persistently broken.
		static constexpr std::chrono::milliseconds loading_restart_cooldown {100};

		/// If a hovered/playing source has no decoded frame for too long, force
		/// recovery. Same network-latency reasoning as loading_restart_timeout.
		static constexpr std::chrono::milliseconds no_frame_restart_timeout {1500};

		VideoHoverPreview();
		~VideoHoverPreview();

		void setup(vulkan_context *vk);
		void shutdown();

		VkDescriptorSet thumbnail(std::string const &source);
		void            notify_hover(std::string const &source);
		bool            save_frame(std::filesystem::path const &path);
		void            tick_idle();

		/// True while a source is actively loaded/playing in the popup (so the caller
		/// can route arrow-key seeks here instead of to the main player).
		[[nodiscard]] bool is_previewing() const noexcept;
		/// Seek the preview's mpv by `seconds` (negative rewinds). No-op when idle.
		/// Safe to call from the UI thread while the worker polls events (mpv async API).
		void               seek_relative(double seconds);

		/// Toggle pause on the currently-playing preview. No-op when idle.
		void                 toggle_pause();
		/// Current playback speed (1.0 = normal). Returns 1.0 when unknown/idle.
		[[nodiscard]] double speed() const;
		/// Set playback speed (used by the Space hold-to-fast-forward FSM).
		void                 set_speed(double s);
		/// Raise/lower preview volume by `delta` (clamped 0..130). Unmutes on raise
		/// and remembers the choice across source reloads without touching the
		/// persisted `preview_sound` config flag.
		void                 adjust_volume(int delta);

		bool               consume_popup_reopen_request();
		[[nodiscard]] bool is_hover_dwell_pending(std::string const &source) const;

		struct GpuSlot {
				VkImage         image {};
				VkDeviceMemory  memory {};
				VkImageView     view {};
				VkSampler       sampler {};
				VkDescriptorSet descriptor {};
				VkImageLayout   layout {VK_IMAGE_LAYOUT_UNDEFINED};
				bool            has_frame = false;
		};

		// core
		void init_mpv();
		void start_thread();
		void stop_thread();

		void load_source(std::string const &source);
		void start_playback(std::string const &source);
		void stop_playback();

		void flush_pending_upload();

		// slot (single texture, no LRU cache)
		bool create_slot();
		void destroy_slot();

		// vulkan
		bool create_shared();
		void destroy_shared();

	private:

		vulkan_context *m_vk {};

		GpuSlot     m_slot {};
		std::string m_slot_source;

		std::string                           m_current;
		std::string                           m_playing;
		std::chrono::steady_clock::time_point m_last_load_time {};
		std::chrono::steady_clock::time_point m_last_use_time {};
		std::chrono::steady_clock::time_point m_last_restart_time {};
		uint64_t                              m_watchdog_restart_count {0};
		uint64_t                              m_idle_stop_count {0};
		std::string                           m_last_restart_source;

		std::string                           m_hovered_source;
		std::chrono::steady_clock::time_point m_hover_start {};

		// Runtime audio state for keyboard volume control. m_user_unmuted overrides
		// the muted-by-default preview so a reload (new hover source) keeps the sound
		// the user dialled in, without writing back to the persisted preview_sound.
		int  m_volume {100};
		bool m_user_unmuted {false};

		mpv_handle         *m_mpv {};
		mpv_render_context *m_render {};

		std::atomic<bool> m_frame_dirty {false};
		std::atomic<bool> m_waiting {false};
		std::atomic<bool> m_upload_pending {false};
		std::atomic<bool> m_popup_reopen_requested {false};

		std::unique_ptr<ManagedThread> m_thread;

		std::vector<uint8_t> m_buf;
		std::mutex           m_buf_mutex;
		std::string          m_pending_source;

		VkBuffer       m_staging {};
		VkDeviceMemory m_staging_mem {};
		void          *m_mapped {};

		VkCommandPool   m_pool {};
		VkCommandBuffer m_cmd {};
		VkFence         m_fence {};

		int m_w {}, m_h {};
};