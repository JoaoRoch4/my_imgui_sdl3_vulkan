#include "pch.hpp"	// NOLINT

#include "file_browser_thumbnail_context_thread.hpp"


// #define THUMB_DEBUG 0
//  #if THUMB_DEBUG
//  #define THUMB_LOG(fmt, ...) std::println("[ThumbVid] " fmt __VA_OPT__(, ) __VA_ARGS__)
//  #else
//  #define THUMB_LOG(fmt, ...) ((void) 0)
//  #endif

#define THUMB_LOG(fmt, ...) ((void)0)

#include <stb_image_write.h>

#include "managed_thread.hpp"
#include "thread_overwatch.hpp"

FileBrowserThumbnailThread::FileBrowserThumbnailThread()
	: k_thumb_w(640), k_thumb_h(480), k_worker_count(std::thread::hardware_concurrency()) {};
FileBrowserThumbnailThread::~FileBrowserThumbnailThread() { shutdown(); }

void FileBrowserThumbnailThread::start(DoneFn on_done) {
	if (!m_workers.empty()) return;
	m_on_done = std::move(on_done);
	m_stopping.store(false, std::memory_order_relaxed);

	// Spawn a small pool: each worker runs the same dequeue->render loop with its own mpv
	// instance, so several video thumbnails render concurrently. The queue mutex already
	// serializes pops, render_video creates a private mpv per job, and m_on_done lands in
	// the context behind m_video_mutex — so no extra synchronization is needed here.
	for (int i = 0; i < k_worker_count; ++i) {
		ManagedThread::Config cfg;
		cfg.name	= "FbThumbVideo" + std::to_string(i);
		cfg.timeout = std::chrono::milliseconds(30'000);  // mpv render can take several s
		cfg.policy	= ThreadOverwatch::RecoveryPolicy::KillOnly;
		cfg.watch	= true;

		m_workers.push_back(std::make_unique<ManagedThread>(
			cfg, [this](std::stop_token const& st, ManagedThread& self) { worker_iteration(st, self); }));
	}
}

void FileBrowserThumbnailThread::shutdown() {
	if (m_workers.empty()) return;
	m_stopping.store(true, std::memory_order_relaxed);
	m_cv.notify_all();	// wake every idle worker so they observe the stop
	for (auto& w : m_workers)
		if (w) w->request_stop();
	m_workers.clear();	// ManagedThread destructors join each worker
	{
		std::lock_guard lk(m_mutex);
		m_queue.clear();
	}
	m_on_done = nullptr;
}

void FileBrowserThumbnailThread::submit(std::string key, std::filesystem::path file, std::filesystem::path out_png) {
	{
		std::lock_guard lk(m_mutex);
		m_queue.push_back({std::move(key), std::move(file), std::move(out_png)});
	}
	m_cv.notify_one();
}

void FileBrowserThumbnailThread::clear_pending() {
	std::lock_guard lk(m_mutex);
	m_queue.clear();
}

void FileBrowserThumbnailThread::worker_iteration(std::stop_token const& st, ManagedThread& self) {
	Job job;
	{
		std::unique_lock lk(m_mutex);
		m_cv.wait_for(lk, st, std::chrono::milliseconds(15'000), [&] {
			return m_stopping.load(std::memory_order_relaxed) || !m_queue.empty() || st.stop_requested();
		});
		if (m_stopping.load(std::memory_order_relaxed) || st.stop_requested() || m_queue.empty()) return;
		job = std::move(m_queue.front());
		m_queue.pop_front();
	}

	self.heartbeat();
	std::vector<std::uint8_t> rgba;
	bool const				  ok = render_video(job.file, job.out_png, rgba, st, self);
	if (m_on_done) m_on_done(job.key, std::move(rgba), ok);
}

namespace {
// True once the buffer holds an actual picture. Early hwdec/post-seek frames read back as
// opaque black (RGB ~0, alpha 255), so an all-zero test misses them — we test the colour
// channels directly. A real frame, even a dark one, has highlights above this floor.
bool frame_has_content(std::vector<std::uint8_t> const& rgba) {
	for (std::size_t i = 0; i + 2 < rgba.size(); i += 4)
		if (rgba[i] > 16 || rgba[i + 1] > 16 || rgba[i + 2] > 16) return true;
	return false;
}
}  // namespace

bool FileBrowserThumbnailThread::render_video(std::filesystem::path const& file, std::filesystem::path const& out_png,
											  std::vector<std::uint8_t>& out_rgba, std::stop_token const& st,
											  ManagedThread& self) {
	mpv_handle* mpv = mpv_create();
	if (!mpv) return false;


	mpv_set_option_string(mpv, "vo", "libmpv");

	mpv_set_option_string(mpv, "profile", "fast");
	// Hardware decode via nvdec-COPY: NVDEC decodes on the GPU but copies each frame back to
	// system memory, which is exactly what the SOFTWARE render API (MPV_RENDER_API_TYPE_SW)
	// needs to read pixels. Plain "nvdec" keeps frames GPU-side and reads back black on this
	// path; nvdec-copy is a hardware-fast decode WITH a valid CPU readback for the thumbnail.
	mpv_set_option_string(mpv, "hwdec", "nvdec-copy");
	mpv_set_option_string(mpv, "mute", "yes");

	mpv_set_option_string(mpv, "gpu-api", "vulkan");

	mpv_set_option_string(mpv, "loop-file", "no");
	mpv_set_option_string(mpv, "cache", "no");
	mpv_set_option_string(mpv, "ytdl", "no");
	mpv_set_option_string(mpv, "msg-level", "all=no");
	mpv_set_option_string(mpv, "demuxer-readahead-secs", "0");


	// --- Redução Gráfica Manual (Foco em GPU) ---
	mpv_set_option_string(mpv, "scale", "bilinear");
	mpv_set_option_string(mpv, "cscale", "bilinear");
	mpv_set_option_string(mpv, "dscale", "bilinear");
	mpv_set_option_string(mpv, "aid", "no");  // Desativa Áudio


	// --- Desativação de Tráfego Paralelo (Opcional, se não precisar deles) ---

	if (mpv_initialize(mpv) < 0) {
		mpv_terminate_destroy(mpv);
		return false;
	}

	std::atomic<bool> frame_ready{false};

	mpv_render_param init_params[] = {
		{MPV_RENDER_PARAM_API_TYPE, static_cast<void*>(const_cast<char*>(MPV_RENDER_API_TYPE_SW))},
		{MPV_RENDER_PARAM_INVALID, nullptr}};

	mpv_render_context* render_ctx = nullptr;
	if (mpv_render_context_create(&render_ctx, mpv, init_params) < 0) {
		mpv_terminate_destroy(mpv);
		return false;
	}

	mpv_render_context_set_update_callback(
		render_ctx, [](void* ctx) { static_cast<std::atomic<bool>*>(ctx)->store(true); }, &frame_ready);

	std::string const path_str = file.string();
	char const*		  cmd[]	   = {"loadfile", path_str.c_str(), nullptr};
	mpv_command(mpv, cmd);
	THUMB_LOG("render start {}", file.filename().string());

	// Phase 1: wait for VIDEO_RECONFIG (file opened).
	bool got_reconfig = false;
	auto deadline	  = std::chrono::steady_clock::now() + std::chrono::seconds(8);
	while (!st.stop_requested() && std::chrono::steady_clock::now() < deadline) {
		self.heartbeat();
		mpv_event* ev = mpv_wait_event(mpv, 0.05);
		if (!ev) break;
		if (ev->event_id == MPV_EVENT_VIDEO_RECONFIG) {
			got_reconfig = true;
			break;
		}
		if (ev->event_id == MPV_EVENT_END_FILE) break;
		if (ev->event_id == MPV_EVENT_SHUTDOWN) break;
	}

	if (!got_reconfig || st.stop_requested()) {
		THUMB_LOG("render NO RECONFIG {} (stop={})", file.filename().string(), st.stop_requested());
		mpv_render_context_free(render_ctx);
		mpv_terminate_destroy(mpv);
		return false;
	}

	// Seek (exact) to ~35 % of duration so the thumbnail is representative instead of an
	// intro/black first frame. Stay paused: a single still needs no playback. Sub-second
	// clips keep the first frame (35 % of them is still ~frame 0).
	double duration = 0.0;
	if (mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &duration) == 0 && duration > 1.0) {
		double const	  seek_pos	 = duration * 0.35;
		std::string const pos_str	 = std::to_string(seek_pos);
		char const*		  seek_cmd[] = {"seek", pos_str.c_str(), "absolute+exact", nullptr};
		mpv_command(mpv, seek_cmd);

		// Block until the seek actually lands. PLAYBACK_RESTART fires once the frame at
		// the new position is decoded and ready — without this wait phase 2 would grab
		// the pre-seek first frame (whose render-update may already be pending).
		auto seek_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!st.stop_requested() && std::chrono::steady_clock::now() < seek_deadline) {
			self.heartbeat();
			mpv_event* ev = mpv_wait_event(mpv, 0.05);
			if (!ev) continue;
			if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART) break;
			if (ev->event_id == MPV_EVENT_END_FILE || ev->event_id == MPV_EVENT_SHUTDOWN) break;
		}
	}

	// Discard any frame-ready latched before/by the seek, then let mpv PLAY. The SW
	// render path only has a real frame to draw once the decoder actually presents one,
	// so we wait for a GENUINE update callback below instead of forcing a render — a
	// forced render while paused draws mpv's empty buffer and yields a black thumbnail.
	frame_ready.store(false, std::memory_order_relaxed);
	mpv_set_property_string(mpv, "pause", "no");

	// Phase 2: render frames as they arrive and keep the first one that holds real
	// picture content. With hwdec the first frame(s) after a seek read back as opaque
	// black before the decoded frame lands — grabbing frame #1 is exactly how the
	// thumbnail ends up black, so we skip black frames until a real one appears.
	bool good = false;
	out_rgba.assign(static_cast<size_t>(k_thumb_w) * k_thumb_h * 4, 0);
	deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

	while (!st.stop_requested() && std::chrono::steady_clock::now() < deadline) {
		self.heartbeat();
		{
			mpv_event* ev = mpv_wait_event(mpv, 0.0);
			if (ev && ev->event_id == MPV_EVENT_END_FILE) break;
		}
		if (frame_ready.exchange(false)) {
			unsigned int	 size[2] = {k_thumb_w, k_thumb_h};
			size_t			 stride	 = static_cast<size_t>(k_thumb_w) * 4;
			mpv_render_param rp[]	 = {{MPV_RENDER_PARAM_SW_SIZE, size},
										{MPV_RENDER_PARAM_SW_FORMAT, static_cast<void*>(const_cast<char*>("rgba"))},
										{MPV_RENDER_PARAM_SW_STRIDE, &stride},
										{MPV_RENDER_PARAM_SW_POINTER, out_rgba.data()},
										{MPV_RENDER_PARAM_INVALID, nullptr}};
			if (mpv_render_context_render(render_ctx, rp) >= 0 && frame_has_content(out_rgba)) {
				good = true;
				break;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	mpv_render_context_free(render_ctx);
	mpv_terminate_destroy(mpv);

	// Only persist/return a frame with real content. A black frame (empty readback or a
	// pre-decode hwdec frame) must never reach disk — the cache would serve it forever.
	if (good && !st.stop_requested()) {
		std::error_code ec;
		std::filesystem::create_directories(out_png.parent_path(), ec);
		stbi_write_png(out_png.string().c_str(), static_cast<int>(k_thumb_w), static_cast<int>(k_thumb_h), 4,
					   out_rgba.data(), static_cast<int>(k_thumb_w) * 4);
		THUMB_LOG("render OK {} -> {}", file.filename().string(), out_png.filename().string());
		return true;
	}
	THUMB_LOG("render NO-FRAME {} (good={} stop={})", file.filename().string(), good, st.stop_requested());
	out_rgba.clear();
	return false;
}
