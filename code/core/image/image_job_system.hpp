#pragma once

#include "pch.hpp"

#include "image_buffer.hpp"
#include "image_ops.hpp"
#include "image_types.hpp"
#include "job_queue.hpp"

// Forward-declared so this header stays free of the project PCH / ManagedThread
// (and thus ImGui/Vulkan). Only image_job_system.cpp pulls those in.
class ManagedThread;

// Process-wide parallel image engine. Owns a JobQueue driven by a pool of
// ManagedThread workers (registered with ThreadOverwatch), and exposes high-level
// decode / resize / encode that run on the pool. It produces ONLY CPU ImageBuffers
// — Vulkan upload stays on the render thread (callers consume the finished buffer).
//
// Threading: start()/shutdown() from the owning thread (typically app init/teardown).
// The high-level ops and submit() are safe to call from any thread.

namespace img {

class ImageJobSystem {
	public:

		struct Config {
				unsigned                  worker_count  = 0; // 0 => hardware_concurrency - reserve_cores
				unsigned                  reserve_cores = 0; // leave cores for render + main
				std::chrono::milliseconds worker_timeout {30'000};
				bool                      watch = true; // register workers with ThreadOverwatch
		};

		ImageJobSystem();
		~ImageJobSystem();
		ImageJobSystem(ImageJobSystem const &)            = delete;
		ImageJobSystem &operator=(ImageJobSystem const &) = delete;

		// Process-wide instance (the app's shared engine).
		static ImageJobSystem &instance();

		void                   start(Config cfg);
		void                   start() { start(Config {}); } // defaults (body context allows {})
		void                   shutdown();
		[[nodiscard]] bool     running() const noexcept { return m_running; }
		[[nodiscard]] unsigned worker_count() const noexcept { return m_worker_count.load(std::memory_order_relaxed); }

		// Generic work submission; returns a future for the result.
		template <class F>
		[[nodiscard]] auto submit(F &&fn, Priority p = Priority::Normal) -> std::future<std::invoke_result_t<F>> {
			return m_queue.submit(std::forward<F>(fn), p);
		}

		// High-level operations, each runs on the pool.
		[[nodiscard]] std::future<std::expected<ImageBuffer, ImageError>>
		decode(std::filesystem::path file, int desired_channels = 4, Priority p = Priority::Normal);

		// Resize; large outputs are tiled across the pool (bit-exact vs single-shot).
		[[nodiscard]] std::future<std::expected<ImageBuffer, ImageError>>
		resize(ImageBuffer src, int dst_w, int dst_h, Priority p = Priority::Normal);

		[[nodiscard]] std::future<std::expected<bool, ImageError>>
		encode_png(ImageBuffer src, std::filesystem::path out, Priority p = Priority::Normal);

		// Drop queued (not-yet-started) jobs — e.g. when navigating away from a folder.
		void clear_pending();

		// Output area at/above which resize is tiled across workers.
		static constexpr long long k_tile_threshold = 256 * 256;

	private:

		JobQueue                                    m_queue;
		std::vector<std::unique_ptr<ManagedThread>> m_workers;
		std::atomic<unsigned>                       m_worker_count {0};
		bool                                        m_running = false;
		Config                                      m_cfg;
};

} // namespace img
