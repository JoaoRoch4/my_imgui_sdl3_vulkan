#include "pch.hpp"

#include "file_browser_thread.hpp"

#include "core/thread/thread_overwatch.hpp"

#include <sys/xattr.h>

namespace {

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

FileBrowserScanner::FileBrowserScanner()
    : m_worker {[this](std::stop_token stoken) { 
		worker_loop(std::move(stoken)); }} { }

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
    m_worker.request_stop();
    m_cv.notify_all();
    if (m_worker.joinable())
	m_worker.join();
}

void FileBrowserScanner::worker_loop(const std::stop_token& stoken) {
    while (!stoken.stop_requested()) {
	Request job;
	{
	    std::unique_lock<std::mutex> lock(m_mutex);
	    m_cv.wait(lock, stoken, [this] { return m_pending.has_value(); });
	    if (stoken.stop_requested())
		break;
	    job = std::move(*m_pending);
	    m_pending.reset();
	}

	m_kill_requested.store(false, std::memory_order_release);

	// Register the watch only for the duration of this scan so idle waiting
	// never trips the watchdog. KillOnly: on hang, set the abort flag and
	// drop the watch — the worker stays alive to serve the next request.
	const std::uint64_t watch_id = ThreadOverwatch::instance().watch(
	    "FileBrowserScanner::scan", std::chrono::milliseconds(5000),
	    [this] { m_kill_requested.store(true, std::memory_order_release); }, nullptr,
	    ThreadOverwatch::RecoveryPolicy::KillOnly);
	m_watch_id.store(watch_id, std::memory_order_release);
			pthread_setname_np(static_cast<pthread_t>(watch_id), "FileBrowserScannerThread");


	bool			ok = true;
	std::string		status;
	std::vector<FileRecord> records = scan(job, stoken, watch_id, ok, status);

	ThreadOverwatch::instance().unwatch(watch_id);
	m_watch_id.store(0, std::memory_order_release);

	if (stoken.stop_requested())
	    break;

	// Publish only if this is still the newest request.
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_latest_gen.load(std::memory_order_acquire) == job.generation) {
	    m_result.generation = job.generation;
	    m_result.records	= std::move(records);
	    m_result.ok		= ok;
	    m_result.status	= std::move(status);
	    m_has_result	= true;
	}
    }
}

std::vector<FileRecord> FileBrowserScanner::scan(const Request& job, const std::stop_token& stoken,
    std::uint64_t watch_id, bool& ok, std::string& status) {
    ok = true;
    status.clear();
    std::vector<FileRecord> records;
		
    ThreadOverwatch::instance().heartbeat(watch_id);

    const auto superseded = [&] {
	return stoken.stop_requested() || m_kill_requested.load(std::memory_order_acquire)
	    || m_latest_gen.load(std::memory_order_acquire) != job.generation;
    };

    try {
	for (const auto& p : std::filesystem::directory_iterator(job.dir)) {
	    if (superseded()) {
		ok = false; // result will be discarded by the caller
		return records;
	    }
	    ThreadOverwatch::instance().heartbeat(watch_id);

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
		    read_xdg_tags(p.path(), rcd.tags);
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
