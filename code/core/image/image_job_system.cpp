#include "pch.hpp" // NOLINT

#include "image_job_system.hpp"


#include "core/thread/managed_thread.hpp"
#include "core/thread/thread_overwatch.hpp"

namespace img {

ImageJobSystem::ImageJobSystem()  = default;
ImageJobSystem::~ImageJobSystem() { shutdown(); }

ImageJobSystem &ImageJobSystem::instance() {
    static ImageJobSystem s_instance;
    return s_instance;
}

void ImageJobSystem::start(Config cfg) {
    if (m_running)
        return;
    m_cfg = cfg;
    m_queue.reset_stop();

    unsigned n = cfg.worker_count;
    if (n == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        n = (hw > cfg.reserve_cores) ? (hw - cfg.reserve_cores) : 1u;
    }
    if (n < 1)
        n = 1;

    m_workers.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        ManagedThread::Config mc;
        mc.name    = "ImageJob" + std::to_string(i);
        mc.timeout = m_cfg.worker_timeout;
        mc.policy  = ThreadOverwatch::RecoveryPolicy::KillOnly; // long jobs must never restart-storm
        mc.watch   = m_cfg.watch;

        // One iteration: wait up to timeout/2 for a job (so idle iterations still
        // return well within the watchdog window), run it, and heartbeat on activity.
        m_workers.push_back(std::make_unique<ManagedThread>(
            mc, [this](const std::stop_token &st, ManagedThread &self) {
                if (m_queue.wait_and_run_one(m_cfg.worker_timeout / 2, st))
                    self.heartbeat();
            }));
    }

    m_worker_count.store(n, std::memory_order_relaxed);
    m_running = true;
}

void ImageJobSystem::shutdown() {
    if (!m_running)
        return;
    m_running = false;
    m_worker_count.store(0, std::memory_order_relaxed);

    m_queue.clear_pending(); // abandon queued jobs (their futures break)
    m_queue.request_stop();  // wake idle workers
    for (auto &w : m_workers)
        if (w)
            w->request_stop();
    m_workers.clear(); // ManagedThread destructors join the workers
}

void ImageJobSystem::clear_pending() { m_queue.clear_pending(); }

std::future<std::expected<ImageBuffer, ImageError>>
ImageJobSystem::decode(std::filesystem::path file, int desired_channels, Priority p) {
    return m_queue.submit(
        [file = std::move(file), desired_channels]() -> std::expected<ImageBuffer, ImageError> {
            return ops::decode_file(file, desired_channels);
        },
        p);
}

std::future<std::expected<ImageBuffer, ImageError>>
ImageJobSystem::resize(ImageBuffer src, int dst_w, int dst_h, Priority p) {
    return m_queue.submit(
        [this, src = std::move(src), dst_w, dst_h]() -> std::expected<ImageBuffer, ImageError> {
            const unsigned hint = m_worker_count.load(std::memory_order_relaxed);

            // Small outputs (thumbnails) resize single-shot — tiling overhead isn't
            // worth it. Large outputs tile across the pool via a parallel_for-backed
            // split executor (bit-exact vs single-shot, proven in image_ops tests).
            if (static_cast<long long>(dst_w) * dst_h < k_tile_threshold || hint <= 1)
                return ops::resize(src, dst_w, dst_h);

            const ops::SplitExecutor exec =
                [this, hint](int total, const std::function<void(int)> &run) {
                    m_queue.parallel_for(
                        0, total, 1,
                        [&run](int lo, int hi) {
                            for (int i = lo; i < hi; ++i) run(i);
                        },
                        hint);
                };
            return ops::resize_parallel(src, dst_w, dst_h, static_cast<int>(hint), exec);
        },
        p);
}

std::future<std::expected<bool, ImageError>>
ImageJobSystem::encode_png(ImageBuffer src, std::filesystem::path out, Priority p) {
    return m_queue.submit(
        [src = std::move(src), out = std::move(out)]() -> std::expected<bool, ImageError> {
            return ops::encode_png(src, out);
        },
        p);
}

std::expected<std::vector<std::byte>, ImageError> ImageJobSystem::encode_bc1(ImageBuffer const &src) {
    const unsigned hint = m_worker_count.load(std::memory_order_relaxed);

    // For very small images (3-row block-grid or smaller) or single-worker pools,
    // fall through to the sequential path — fan-out overhead beats the savings.
    const int by = (src.height + 3) / 4;
    if (by <= 1 || hint <= 1)
        return ops::encode_bc1(src);

    const ops::SplitExecutor exec =
        [this, hint](int total, const std::function<void(int)> &run) {
            m_queue.parallel_for(
                0, total, 1,
                [&run](int lo, int hi) {
                    for (int i = lo; i < hi; ++i) run(i);
                },
                hint);
        };
    // max_splits caps at worker_count: more workers than we have cores wastes
    // scheduling overhead. parallel_for's calling-thread helper keeps us nest-safe.
    return ops::encode_bc1_parallel(src, static_cast<int>(hint), exec);
}

} // namespace img
