#pragma once

#include "pch.hpp" // ImTextureID

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "file_browser_thumbnail_context_thread.hpp"
#include "image_buffer.hpp"
#include "image_types.hpp"

class vulkan_context;
class VulkanTexture;

// Render-thread front-end of the file-browser thumbnail engine. Owns the cache state
// (path -> texture), performs all GPU uploads, and applies the anti-lag policies:
//   * cheap string key (currentDirectory_ is already absolute -> no per-frame syscall);
//   * a per-frame GPU-upload budget so a fresh grid can't stall a frame;
//   * deferred texture frees (retire-after-N-frames) instead of vkDeviceWaitIdle;
//   * in-memory RGBA handoff (no PNG re-decode on the hot path for fresh thumbnails).
//
// Image thumbnails are generated on the shared ImageJobSystem pool (one composite
// decode->resize->encode job per file); video thumbnails are produced by the owned
// FileBrowserThumbnailThread (libmpv). Both deliver an RGBA ImageBuffer that this class
// uploads via VulkanTexture::upload on the render thread. All public methods MUST be
// called from the render thread.
class FileBrowserThumbnailContext {
	public:

		FileBrowserThumbnailContext();
		~FileBrowserThumbnailContext();
		FileBrowserThumbnailContext(FileBrowserThumbnailContext const &)            = delete;
		FileBrowserThumbnailContext &operator=(FileBrowserThumbnailContext const &) = delete;

		void               setup(vulkan_context *vk, std::filesystem::path thumb_dir);
		void               shutdown();
		[[nodiscard]] bool is_setup() const noexcept { return m_setup; }

		// Per-frame hook: reset the upload budget, tick the retire queue, drain video results.
		void begin_frame();

		// Non-blocking. Returns the texture id, or 0 while generating/uploading.
		[[nodiscard]] ImTextureID get(std::filesystem::path const &path);

		// Hot-path overload: the caller supplies the cache key and classification it
		// precomputed once per directory scan (FileRecord::thumbKey/isVideoThumb), so a
		// cached thumbnail costs one heterogeneous hash lookup — no path normalize, no
		// extension parse, no string allocation. `dir`/`name` are touched only on the
		// first sighting of a file (cache miss), where the full path is joined to submit.
		[[nodiscard]] ImTextureID
		get(std::string_view key, std::filesystem::path const &dir, std::filesystem::path const &name, bool is_video);

		void evict(std::filesystem::path const &path); // regenerate next get()
		void clear(); // drop everything + delete PNGs

		// True for files we generate thumbnails for (images + videos).
		[[nodiscard]] static bool is_thumbnailable(std::filesystem::path const &path);

		// One-shot classification used to fill FileRecord's thumbnail fast-path at scan time.
		struct Classification {
				bool thumbnailable = false;
				bool is_video      = false;
		};
		[[nodiscard]] static Classification classify(std::filesystem::path const &path);

		// The lexically-normalized cache key for a path (matches key_for); public so the
		// scan-time precompute can build it off the render hot path.
		[[nodiscard]] static std::string make_key(std::filesystem::path const &path);

		static constexpr int k_thumb_w               = 320;
		static constexpr int k_thumb_h               = 180;
		static constexpr int k_max_uploads_per_frame = 4;
		static constexpr int k_retire_frames         = 3;
		// Cap on live GPU thumbnail textures. Beyond this, the least-recently-used are
		// retired so a folder with thousands of files can't exhaust samplers/descriptors/
		// VRAM. Must comfortably exceed the number of thumbnails visible at once.
		static constexpr int k_max_live_textures     = 4096 * 3;

	private:

		// Queued      -> never seen, needs first generation
		// Generating  -> a decode/mpv/PNG-reload job is in flight
		// PixelsReady -> RGBA in hand, awaiting GPU upload
		// Ready       -> live texture on the GPU
		// Cached      -> PNG persisted on disk, texture evicted to honour the VRAM cap;
		//                re-display is a cheap PNG decode, NOT a source regeneration
		// Failed      -> generation failed; do not retry
		enum class State { Queued, Generating, PixelsReady, Ready, Cached, Failed };

		using ImgResult = std::expected<img::ImageBuffer, img::ImageError>;

		struct Entry {
				State                          state = State::Queued;
				std::filesystem::path          png_path;
				bool                           is_video = false;
				std::future<ImgResult>         img_future; // image path: composite ImageJobSystem job
				img::ImageBuffer               pixels; // PixelsReady: RGBA awaiting GPU upload
				bool                           have_pixels = false;
				std::unique_ptr<VulkanTexture> texture;
				std::uint64_t                  last_used = 0; // frame index of last get() — for LRU
		};

		struct Retire {
				std::unique_ptr<VulkanTexture> texture;
				int                            frames_left;
		};

		// Transparent hasher so m_entries.find(std::string_view) needs no temporary string.
		struct StringHash {
				using is_transparent = void;
				[[nodiscard]] std::size_t operator()(std::string_view s) const noexcept {
					return std::hash<std::string_view> {}(s);
				}
		};

		[[nodiscard]] std::string           key_for(std::filesystem::path const &path) const;
		[[nodiscard]] std::filesystem::path png_for(std::string const &key) const;
		void                                submit_image(std::filesystem::path const &file, Entry &e);
		// Per-frame state advance (future poll -> upload-budget -> texture id) shared by both
		// get() overloads; assumes last_used was already stamped by the caller.
		[[nodiscard]] ImTextureID           poll_entry(Entry &e);
		void on_video_done(std::string const &key, std::vector<std::uint8_t> rgba, bool ok);
		void enforce_texture_cap(); // retire least-recently-used textures past k_max_live_textures

		vulkan_context       *m_vk = nullptr;
		std::filesystem::path m_thumb_dir;
		bool                  m_setup = false;

		std::unordered_map<std::string, Entry, StringHash, std::equal_to<>> m_entries; // render-thread only
		std::vector<Retire>                                                 m_retire;
		int                                                                 m_uploads_this_frame = 0;
		std::uint64_t m_frame = 0; // monotonic frame index (LRU clock)

		FileBrowserThumbnailThread                     m_video;
		// Video worker delivers results here (worker thread); drained on begin_frame.
		std::mutex                                     m_video_mutex;
		std::vector<std::pair<std::string, ImgResult>> m_video_results;
};
