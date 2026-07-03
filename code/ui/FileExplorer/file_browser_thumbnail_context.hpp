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
		// @p image_tier / @p video_tier are quality presets ("original"|"high"|"medium"|"low")
		// bundling a resolution cap (and, for video, the BC1 letterbox size). See setup().
		void               setup(vulkan_context *vk, std::filesystem::path thumb_dir,
						  std::string thumbnail_format = "bc1", std::string image_tier = "original",
						  std::string video_tier = "medium");
		void               shutdown();
		[[nodiscard]] bool is_setup() const noexcept { return m_setup; }

		// Inline animated-GIF playback policy (a runtime "run config"):
		//   HoverOnly  - only the hovered GIF animates; others show the still first frame.
		//   AllVisible - every visible GIF loops (heaviest: more frame RAM + re-uploads).
		//   Hybrid     - all GIFs loop, but non-hovered ones advance at a throttled rate.
		enum class GifPlayback { HoverOnly, AllVisible, Hybrid };
		void set_gif_playback(GifPlayback mode) noexcept { m_gif_playback = mode; }
		[[nodiscard]] GifPlayback gif_playback() const noexcept { return m_gif_playback; }
		// UI hint (per frame, before begin_frame): the GIF cache key currently hovered.
		// Consumed next frame so HoverOnly/Hybrid know which GIF gets full-rate animation.
		void                      note_gif_hover(std::string_view key);

		// Drop all live animated-GIF state (current-frame textures + decoded frames). The next
		// get() for each GIF re-evaluates under the current playback mode (re-decoding frames if
		// it should animate). Call when the playback mode changes so it takes effect at once.
		void refresh_gifs();

		// Per-frame hook: reset the upload budget, tick the retire queue, drain video results.
		// `dt_seconds` advances the GIF animation clocks (pass ImGui::GetIO().DeltaTime).
		void begin_frame(float dt_seconds = 0.0f);

		// Non-blocking. Returns the texture id, or 0 while generating/uploading.
		[[nodiscard]] ImTextureID get(std::filesystem::path const &path);

		// Hot-path overload: the caller supplies the cache key and classification it
		// precomputed once per directory scan (FileRecord::thumbKey/isVideoThumb), so a
		// cached thumbnail costs one heterogeneous hash lookup — no path normalize, no
		// extension parse, no string allocation. `dir`/`name` are touched only on the
		// first sighting of a file (cache miss), where the full path is joined to submit.
		[[nodiscard]] ImTextureID get(std::string_view key, std::filesystem::path const &dir,
			std::filesystem::path const &name, bool is_video, bool is_gif = false);

		void evict(std::filesystem::path const &path); // regenerate next get()
		void clear(); // drop everything + delete PNGs
		// Free all live GPU textures + in-memory entries (keeps the on-disk caches). Call on a
		// directory switch so the previous folder's thumbnail VRAM is reclaimed.
		void release_textures();

		// True for files we generate thumbnails for (images + videos).
		[[nodiscard]] static bool is_thumbnailable(std::filesystem::path const &path);

		// One-shot classification used to fill FileRecord's thumbnail fast-path at scan time.
		struct Classification {
				bool thumbnailable = false;
				bool is_video      = false;
				bool is_gif        = false; // animated-GIF candidate (inline playback)
		};
		[[nodiscard]] static Classification classify(std::filesystem::path const &path);

		// The lexically-normalized cache key for a path (matches key_for); public so the
		// scan-time precompute can build it off the render hot path.
		[[nodiscard]] static std::string make_key(std::filesystem::path const &path);

		// Default VIDEO thumbnail letterbox size (the "medium" tier). The live values are
		// m_thumb_w/m_thumb_h, set from the video quality tier in setup().
		static constexpr uint64_t k_thumb_w_default        = std::numeric_limits<uint64_t>::max();
		static constexpr uint64_t k_thumb_h_default        = std::numeric_limits<uint64_t>::max();
		// Image thumbnails decode at NATIVE resolution (full quality, matching the hover
		// preview) instead of the k_thumb_w x k_thumb_h letterbox; m_image_max_edge caps the
		// long edge purely as a VRAM guard for huge sources. Masonry cells never exceed the
		// window width, so it is visually lossless at display size. Videos ignore it.
		// Auto-sized from available VRAM in setup(); this is the fallback when the GPU can't
		// report its memory.
		static constexpr uint64_t k_image_max_edge_default = 2048;
		static constexpr uint64_t k_max_uploads_per_frame  = 4;
		static constexpr uint64_t k_retire_frames          = 3;
		// Cap on live GPU thumbnail textures. Beyond this, the least-recently-used are
		// retired so a folder with thousands of files can't exhaust samplers/descriptors/
		// VRAM. Must comfortably exceed the number of thumbnails visible at once.
		static constexpr uint64_t k_max_live_textures      = std::numeric_limits<uint64_t>::max();

		[[nodiscard]] bool
		GetLoadedTextureDimensions(std::string const &key, int &outW, int &outH) const noexcept;

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

		using ImgResult = std::expected<img::ImageBuffer, img::ImageError>;
		using Bc1Result = std::expected<std::vector<std::byte>, img::ImageError>;

		struct Entry {
				State                  state = State::Queued;
				std::filesystem::path  png_path;
				bool                   is_video        = false;
				// Source type + fallback bookkeeping: when a fresh VIDEO generate
				// (ffmpegthumbnailer) fails, poll_entry re-submits the source to the Reproducer's
				// mpv (nvdec-copy) worker exactly once. source_is_video records the source kind
				// (distinct from is_video, which gates future polling); tried_mpv prevents an
				// infinite re-derive loop.
				bool                   source_is_video = false;
				bool                   tried_mpv       = false;
				// Per-entry storage route (decided in get(), not global). The backend is no
				// longer one-size-fits-all: BC1's 4-colour-per-block quantization bands badly on
				// flat-shaded art, so only VIDEO thumbs take it (motion hides it + decode is the
				// real cost). Images decode to lossless RGBA and are NOT persisted to disk —
				// stb decode is cheap enough that a thumbnail cache buys nothing.
				//   use_bc1 == true  -> video, BC1 blob cache (m_backend == Bc1)
				//   use_bc1 == false -> RGBA upload; png_path set only for the no-BC video fallback
				bool                   use_bc1         = false;
				std::future<ImgResult> img_future; // image path: composite ImageJobSystem job
				img::ImageBuffer       pixels; // PixelsReady: RGBA awaiting GPU upload
				bool                   have_pixels = false;
				std::unique_ptr<VulkanTexture> texture;
				std::uint64_t                  last_used = 0; // frame index of last get() — for LRU
				std::uint64_t ready_frame = 0; // frame it became PixelsReady — upload fairness
				// BC1 backend: blocks delivered by the pool job / blob lookup, uploaded via
				// upload_bc1. have_bc1 mirrors have_pixels; from_blob skips the re-store.
				std::future<Bc1Result> bc1_future;
				std::vector<std::byte> bc1_blocks;
				bool                   have_bc1  = false;
				bool                   from_blob = false;
				std::uint64_t          blob_key  = 0;
		};

		struct Retire {
				std::unique_ptr<VulkanTexture> texture;
				int                            frames_left;
		};

		// ── Inline animated-GIF playback ──────────────────────────────────────────
		// Decoded frame set for one GIF (produced off-thread on the ImageJobSystem pool).
		// `rgba` is count contiguous w*h*4 frames; delays are per-frame, in milliseconds.
		struct GifFrames {
				int                       w     = 0;
				int                       h     = 0;
				int                       count = 0;
				std::vector<std::uint8_t> rgba;
				std::vector<int>          delays_ms;
				[[nodiscard]] bool ok() const noexcept { return count > 0 && w > 0 && h > 0; }
		};

		// Live animation state for one GIF: frames in CPU RAM + the current-frame texture.
		// Only the current frame ever resides in VRAM (one texture, re-uploaded when the
		// frame advances), so a GIF costs the same VRAM as a still thumbnail.
		struct GifAnim {
				enum class St { Decoding, Ready, Failed };
				St                     state = St::Decoding;
				std::future<GifFrames> future;
				GifFrames              frames;
				double                 accum_ms    = 0.0; // time into the current frame
				int                    cur         = 0; // current frame index
				int                    uploaded    = -1; // frame index now in `texture`
				bool                   dirty       = false; // cur not yet uploaded to texture
				std::uint64_t          last_upload = 0; // frame of last upload — fairness key
				std::unique_ptr<VulkanTexture> texture;
				std::uint64_t last_used = 0; // frame index of last get() — for LRU prune
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
		// Per-frame state advance for one entry: poll the decode future to PixelsReady, then return
		// the live texture id (or 0). The GPU upload itself is NOT done here — it is batched into
		// flush_image_uploads() so a screenful of ready thumbnails uploads fairly, not in draw order.
		[[nodiscard]] ImTextureID poll_entry(Entry &e);
		// Fair, bounded upload of every PixelsReady still thumbnail (longest-waiting first), run in
		// begin_frame with the k_max_uploads_per_frame budget — the same decoupled scheduling the
		// animated GIFs use (flush_gif_uploads), so visible thumbnails aren't starved by draw order.
		void                      flush_image_uploads();
		void on_video_done(std::string const &key, std::vector<std::uint8_t> rgba, bool ok);
		void enforce_texture_cap(); // retire least-recently-used textures past k_max_live_textures

		// Animated-GIF helpers (render thread).
		static GifFrames   decode_gif_frames(std::filesystem::path const &file); // stb multi-frame
		[[nodiscard]] bool gif_should_animate(std::string_view key) const; // per mode + hover
		// Advance/upload the GIF for `key`, decoding async on first sight. Returns the current
		// frame's texture id, or 0 while still decoding (caller falls back to the still thumb).
		[[nodiscard]] ImTextureID
			 tick_gif(std::string_view key, std::filesystem::path const &file, bool hovered);
		// Upload the current frame of every "dirty" GIF, fairly (longest-waiting first) and bounded
		// by k_max_gif_uploads_per_frame. Runs in begin_frame, off the greedy per-row get() path,
		// so no GIF is starved by the ones drawn before it. Decoupled from the still-upload budget.
		void flush_gif_uploads();
		void prune_gifs(); // bound GIF frame RAM: retire least-recently-used GIFs

		vulkan_context       *m_vk = nullptr;
		std::filesystem::path m_thumb_dir;
		bool                  m_setup = false;

		Backend            m_backend = Backend::Png; // resolved in setup()
		uint64_t           m_image_max_edge; // auto-sized from VRAM + image tier
		uint64_t           m_thumb_w; // video BC1 letterbox size (video tier)
		uint64_t           m_thumb_h;
		ThumbnailBlobCache m_blob; // BC1 backend store (single blob + index)

		std::unordered_map<std::string, Entry, StringHash, std::equal_to<>> m_entries; // render-thread
																					   // only
		std::vector<Retire> m_retire;
		uint64_t            m_uploads_this_frame = 0;
		std::uint64_t       m_frame              = 0; // monotonic frame index (LRU clock)

		// Animated-GIF state (render-thread only). Keyed by the same cache key as m_entries.
		std::unordered_map<std::string, GifAnim, StringHash, std::equal_to<>> m_gifs;
		GifPlayback m_gif_playback = GifPlayback::HoverOnly;
		std::string m_hovered_gif; // key animating at full rate this frame (HoverOnly/Hybrid)
		std::string m_hovered_gif_next; // collected from UI this frame, promoted on begin_frame
		double      m_dt_ms                          = 0.0; // last frame delta, for GIF clocks
		// Cap on GIFs whose frames stay resident in RAM; LRU-pruned past this.
		static constexpr std::size_t k_max_gif_anims = 64;
		// GIF frame uploads get their OWN per-frame budget (separate from the still-thumbnail
		// k_max_uploads_per_frame) so a screen of GIFs never starves behind still uploads. The
		// budget is spent fairly (longest-waiting GIF first) so none freezes — see
		// flush_gif_uploads().
		static constexpr std::size_t k_max_gif_uploads_per_frame = 8;

		// Reload cached PNGs (stb) + owns the nvdec-copy mpv worker used as the video
		// re-derive fallback. The worker delivers results via on_video_done.
		ThumbnailReproducer                            m_reproducer;
		// mpv worker delivers results here (worker thread); drained on begin_frame.
		std::mutex                                     m_video_mutex;
		std::vector<std::pair<std::string, ImgResult>> m_video_results;
};
