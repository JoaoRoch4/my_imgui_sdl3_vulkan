#include "pch.hpp"

#include "file_browser_thread.hpp"

#include "managed_thread.hpp"

#include "image_ops.hpp" // img::ops::probe_dimensions — AVIF/HEIF size (stbi_info can't)

#include <fcntl.h>     // AT_FDCWD
#include <sys/stat.h>  // statx, STATX_BTIME
#include <sys/xattr.h>

#include <cstdint>

// Header-only image-dimension probe used during scan to feed the masonry view its
// per-record aspect ratio.  stb_image's stbi_info reads only the file header — no
// decode — so it's cheap enough to run on every image during a scan.
#include <stb_image.h>

namespace {

/// File birth (creation) time in nanoseconds since the Unix epoch, via statx's
/// STATX_BTIME. Returns 0 when the kernel/filesystem doesn't record a birth time
/// (e.g. some network/FUSE mounts) so those entries simply sort as "oldest".
std::int64_t creation_time_ns(const std::filesystem::path& p) {
    struct statx stx{};
    if (statx(AT_FDCWD, p.c_str(), AT_STATX_SYNC_AS_STAT, STATX_BTIME, &stx) != 0)
	return 0;
    if (!(stx.stx_mask & STATX_BTIME))
	return 0; // birth time unsupported on this filesystem
    return static_cast<std::int64_t>(stx.stx_btime.tv_sec) * 1'000'000'000LL +
	   static_cast<std::int64_t>(stx.stx_btime.tv_nsec);
}

/// Convert a path's UTF-8 representation to std::string (char8_t-safe).
std::string u8_to_string(const std::filesystem::path& p) {
#if defined(__cpp_lib_char8_t)
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
#else
    return p.u8string();
#endif
}

/// Read Dolphin-compatible user.xdg.tags xattr into `out` (comma-separated,
/// trimmed). No-op when the attribute is absent.
void read_xdg_tags(const std::filesystem::path& path, std::vector<std::string>& out) {
    const ssize_t xsz = getxattr(path.c_str(), "user.xdg.tags", nullptr, 0);
    if (xsz <= 0)
	return;

    std::string raw(static_cast<std::size_t>(xsz), '\0');
    if (getxattr(path.c_str(), "user.xdg.tags", raw.data(), raw.size()) < 0)
	return;

    std::size_t s = 0;
    while (s < raw.size()) {
	std::size_t e = raw.find(',', s);
	if (e == std::string::npos)
	    e = raw.size();
	std::string tag = raw.substr(s, e - s);
	while (!tag.empty() && (tag.front() == ' ' || tag.front() == '\t'))
	    tag.erase(tag.begin());
	while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t'))
	    tag.pop_back();
	if (!tag.empty())
	    out.push_back(std::move(tag));
	s = e + 1;
    }
}

} // namespace

FileBrowserScanner::FileBrowserScanner() {
    ManagedThread::Config cfg;
    cfg.name    = "FileBrowserScan";
    cfg.timeout = std::chrono::milliseconds(5000);
    cfg.policy  = ThreadOverwatch::RecoveryPolicy::KillOnly;
    m_worker    = std::make_unique<ManagedThread>(
	cfg, [this](const std::stop_token& st, ManagedThread& self) { worker_iteration(st, self); });
}

FileBrowserScanner::~FileBrowserScanner() { shutdown(); }

std::uint64_t FileBrowserScanner::request(std::filesystem::path dir, bool skip_errors) {
    std::uint64_t gen = 0;
    {
	std::lock_guard<std::mutex> lock(m_mutex);
	gen = m_latest_gen.load(std::memory_order_relaxed) + 1;
	m_latest_gen.store(gen, std::memory_order_release);
	m_pending    = Request {std::move(dir), skip_errors, gen};
	m_has_result = false; // any previously ready result is now stale
    }
    m_cv.notify_one();
    return gen;
}

bool FileBrowserScanner::poll(Result& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_has_result)
	return false;
    // Only hand back the result for the newest request.
    if (m_result.generation != m_latest_gen.load(std::memory_order_acquire)) {
	m_has_result = false;
	return false;
    }
    out		 = std::move(m_result);
    m_has_result = false;
    return true;
}

void FileBrowserScanner::shutdown() {
    {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_pending.reset();
    }
    if (m_worker) {
	m_worker->request_stop();
	m_cv.notify_all();   // wake the bounded wait so the join returns promptly
	m_worker.reset();    // ManagedThread destructor joins
    }
}

// One iteration of the scanner loop. ManagedThread owns the loop, names the thread
// ("FileBrowserScan"), and watches it for the whole lifetime (KillOnly). The bounded
// wait keeps the watchdog fed while idle; scan() heartbeats per entry so a legitimately
// long scan is not mistaken for a hang. On a true hang the watchdog calls request_stop,
// which superseded() observes via the stop_token.
void FileBrowserScanner::worker_iteration(const std::stop_token& stoken, ManagedThread& self) {
    Request job;
    {
	std::unique_lock<std::mutex> lock(m_mutex);
	m_cv.wait_for(lock, std::chrono::milliseconds(2000),
		      [this, &stoken] { return stoken.stop_requested() || m_pending.has_value(); });
	if (stoken.stop_requested() || !m_pending.has_value())
	    return; // idle timeout or shutdown — heartbeat happens next iteration
	job = std::move(*m_pending);
	m_pending.reset();
    }

    bool			ok = true;
    std::string			status;
    std::vector<FileRecord> records = scan(job, stoken, self, ok, status);

    if (stoken.stop_requested())
	return;

    // Publish only if this is still the newest request.
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_latest_gen.load(std::memory_order_acquire) == job.generation) {
	m_result.generation = job.generation;
	m_result.records	= std::move(records);
	m_result.ok		= ok;
	m_result.status		= std::move(status);
	m_has_result		= true;
    }
}

std::vector<FileRecord> FileBrowserScanner::scan(const Request& job, const std::stop_token& stoken,
    ManagedThread& self, bool& ok, std::string& status) {
    ok = true;
    status.clear();
    std::vector<FileRecord> records;

    self.heartbeat();

    const auto superseded = [&] {
	return stoken.stop_requested()
	    || m_latest_gen.load(std::memory_order_acquire) != job.generation;
    };

    try {
	for (const auto& p : std::filesystem::directory_iterator(job.dir)) {
	    if (superseded()) {
		ok = false; // result will be discarded by the caller
		return records;
	    }
	    self.heartbeat();

	    FileRecord rcd;
	    try {
		if (p.is_regular_file())
		    rcd.isDir = false;
		else if (p.is_directory())
		    rcd.isDir = true;
		else
		    continue;

		rcd.name = p.path().filename();
		if (rcd.name.empty())
		    continue;

		rcd.extension = p.path().filename().extension();
		rcd.showName  = (rcd.isDir ? "[D] " : "[F] ") + u8_to_string(p.path().filename());

		if (!rcd.isDir) {
		    std::error_code ec;
		    rcd.size = p.file_size(ec);
		    if (ec)
			rcd.size = 0;
		    rcd.lastWriteTime = p.last_write_time(ec);
		    if (ec)
			rcd.lastWriteTime = {};
		    rcd.creationTime = creation_time_ns(p.path());
		    read_xdg_tags(p.path(), rcd.tags);

		    // Native source dimensions for the masonry layout.  Only call
		    // stbi_info() on extensions it actually understands — videos and
		    // other formats leave source_w/h at 0 so the masonry renderer
		    // falls back to its default 16:9 cell aspect for those.
		    std::string ext_lc = rcd.extension.string();
		    for (char& c : ext_lc)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		    const bool is_stb_image = ext_lc == ".jpg" || ext_lc == ".jpeg" ||
					      ext_lc == ".png" || ext_lc == ".bmp" ||
					      ext_lc == ".gif" || ext_lc == ".tga" ||
					      ext_lc == ".psd" || ext_lc == ".hdr" ||
					      ext_lc == ".pic" || ext_lc == ".pnm" ||
					      ext_lc == ".ppm" || ext_lc == ".pgm";
		    const bool is_ffmpeg_image = ext_lc == ".avif" || ext_lc == ".heic" || ext_lc == ".heif";
		    if (is_stb_image) {
			int sw = 0, sh = 0, sc = 0;
			if (stbi_info(p.path().string().c_str(), &sw, &sh, &sc) == 1 &&
			    sw > 0 && sh > 0) {
			    rcd.source_w = sw;
			    rcd.source_h = sh;
			}
		    } else if (is_ffmpeg_image) {
			// stbi_info can't parse AVIF/HEIF; probe the header via FFmpeg so the
			// masonry view sizes the cell to the true aspect, not the 16:9 fallback.
			if (auto const d = img::ops::probe_dimensions(p.path())) {
			    rcd.source_w = d->first;
			    rcd.source_h = d->second;
			}
		    }
		}
	    } catch (...) {
		if (!job.skip_errors) {
		    ok	   = false;
		    status = "error: failed to read a directory entry";
		    return records;
		}
		continue;
	    }
	    records.push_back(std::move(rcd));
	}
    } catch (const std::exception& err) {
	ok     = false;
	status = std::string("error: ") + err.what();
	records.clear();
    } catch (...) {
	ok     = false;
	status = "unknown error";
	records.clear();
    }

    return records;
}
