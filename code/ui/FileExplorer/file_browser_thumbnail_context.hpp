#pragma once

#include "pch.hpp" // ImTextureID

#include "image_buffer.hpp"
#include "image_types.hpp"
#include "thumbnail_blob_cache.hpp" // BC1 backend: single-blob store + index
#include "thumbnail_reproducer.hpp" // owns the nvdec-copy mpv worker; pulls in the thread header

class vulkan_context;
class VulkanTexture;

// Render-thread front-end of the file-browser thumbnail engine. Owns the cache state
// (path -> texture), performs all GPU uploads, and applies the anti-lag policies:
//   * cheap string key (currentDirectory_ is already absolute -> no per-frame syscall);
//   * a per-frame GPU-upload budget so a fresh grid can't stall a frame;
//   * deferred texture frees (retire-after-N-frames) instead of vkDeviceWaitIdle;
//   * in-memory RGBA handoff (no PNG re-decode on the hot path for fresh thumbnails).
//
// Thumbnails are produced on the shared ImageJobSystem pool by two single-responsibility
// engines: ThumbnailGenerator creates a thumbnail from a source on a cache miss (video ->
// ffmpegthumbnailer, image/gif -> stb) and persists a PNG; ThumbnailReproducer reloads an
// existing cached PNG via stb on a cache hit. A video the Generator can't decode falls back
// to the Reproducer's nvdec-copy mpv worker (async, delivered via on_video_done). Every path
// yields an RGBA ImageBuffer this class uploads via VulkanTexture::upload on the render
// thread. All public methods MUST be called from the render thread.
class FileBrowserThumbnailContext {
	public:

		FileBrowserThumbnailContext();
		~FileBrowserThumbnailContext();
		FileBrowserThumbnailContext(FileBrowserThumbnailContext const &)            = delete;
		FileBrowserThumbnailContext &operator=(FileBrowserThumbnailContext const &) = delete;

		// @p thumbnail_format selects storage: "bc1" (GPU block-compressed single-blob,
		// default) or "png" (per-file fallback). bc1 auto-falls-back to png when the
		// device lacks textureCompressionBC.
		void               setup(vulkan_context *vk, std::filesystem::path thumb_dir,
		                         std::string thumbnail_format = "bc1");
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

		static constexpr int k_thumb_w               = 640;
		static constexpr int k_thumb_h               = 480;
		static constexpr int k_max_uploads_per_frame = 4;
		static constexpr int k_retire_frames         = 3;
		// Cap on live GPU thumbnail textures. Beyond this, the least-recently-used are
		// retired so a folder with thousands of files can't exhaust samplers/descriptors/
		// VRAM. Must comfortably exceed the number of thumbnails visible at once.
		static constexpr int k_max_live_textures     = 1024 *8;

	private:

		// Queued      -> never seen, needs first generation
		// Generating  -> a decode/mpv/PNG-reload job is in flight
		// PixelsReady -> RGBA in hand, awaiting GPU upload
		// Ready       -> live texture on the GPU
		// Cached      -> PNG persisted on disk, texture evicted to honour the VRAM cap;
		//                re-display is a cheap PNG decode, NOT a source regeneration
		// Failed      -> generation failed; do not retry
		enum class State { Queued, Generating, PixelsReady, Ready, Cached, Failed };

		// Storage backend resolved at setup().
		enum class Backend { Png, Bc1 };

		using ImgResult  = std::expected<img::ImageBuffer, img::ImageError>;
		using Bc1Result  = std::expected<std::vector<std::byte>, img::ImageError>;

		struct Entry {
				State                          state = State::Queued;
				std::filesystem::path          png_path;
				bool                           is_video = false;
				// Source type + fallback bookkeeping: when a fresh VIDEO generate (ffmpegthumbnailer)
				// fails, poll_entry re-submits the source to the Reproducer's mpv (nvdec-copy) worker
				// exactly once. source_is_video records the source kind (distinct from is_video, which
				// gates future polling); tried_mpv prevents an infinite re-derive loop.
				bool                           source_is_video = false;
				bool                           tried_mpv       = false;
				std::future<ImgResult>         img_future; // image path: composite ImageJobSystem job
				img::ImageBuffer               pixels; // PixelsReady: RGBA awaiting GPU upload
				bool                           have_pixels = false;
				std::unique_ptr<VulkanTexture> texture;
				std::uint64_t                  last_used = 0; // frame index of last get() — for LRU
				// BC1 backend: blocks delivered by the pool job / blob lookup, uploaded via
				// upload_bc1. have_bc1 mirrors have_pixels; from_blob skips the re-store.
				std::future<Bc1Result>         bc1_future;
				std::vector<std::byte>         bc1_blocks;
				bool                           have_bc1   = false;
				bool                           from_blob  = false;
				std::uint64_t                  blob_key   = 0;
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
		// Decode -> letterbox(320x180) -> encode PNG -> deliver via e.img_future, on the
		// ImageJobSystem pool. `decode_as_video` picks ffmpegthumbnailer (smart frame, fast
		// CPU decode) over decode_file() for a fresh video source; both letterbox identically.
		void submit_image(std::filesystem::path const &file, Entry &e, bool decode_as_video);
		// Per-frame state advance (future poll -> upload-budget -> texture id) shared by both
		// get() overloads; assumes last_used was already stamped by the caller.
		[[nodiscard]] ImTextureID           poll_entry(Entry &e);
		void on_video_done(std::string const &key, std::vector<std::uint8_t> rgba, bool ok);
		void enforce_texture_cap(); // retire least-recently-used textures past k_max_live_textures

		vulkan_context       *m_vk = nullptr;
		std::filesystem::path m_thumb_dir;
		bool                  m_setup = false;

		Backend            m_backend = Backend::Png; // resolved in setup()
		ThumbnailBlobCache m_blob;                   // BC1 backend store (single blob + index)

		std::unordered_map<std::string, Entry, StringHash, std::equal_to<>> m_entries; // render-thread only
		std::vector<Retire>                                                 m_retire;
		int                                                                 m_uploads_this_frame = 0;
		std::uint64_t m_frame = 0; // monotonic frame index (LRU clock)

		// Reload cached PNGs (stb) + owns the nvdec-copy mpv worker used as the video
		// re-derive fallback. The worker delivers results via on_video_done.
		ThumbnailReproducer                            m_reproducer;
		// mpv worker delivers results here (worker thread); drained on begin_frame.
		std::mutex                                     m_video_mutex;
		std::vector<std::pair<std::string, ImgResult>> m_video_results;
};
