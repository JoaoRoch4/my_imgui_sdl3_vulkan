#include "pch.hpp"

#include "video_downloader.hpp"
#include "core/log/debug_log.hpp"
#include "managed_thread.hpp"

namespace {
// Browser User-Agent for the curl fallback — some CDNs (e.g. the boomio
// redirect target) gate direct fetches on a non-curl User-Agent.
constexpr const char *kFallbackUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36";
} // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint64_t VideoDownloader::fnv1a(const std::string &s) {
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::filesystem::path VideoDownloader::cache_path_for(const std::string &url) const {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(fnv1a(url)));
    return m_cache_dir / (std::string(hex) + ".mp4");
}

std::filesystem::path VideoDownloader::python_fallback_script() const {
    if (m_cache_dir.empty())
        return {};
    // m_cache_dir == <repo>/build/cache/video_cache, so the repo root is three
    // parents up; the fallback script lives at <repo>/scripts/video_download.py.
    const auto repo_root = m_cache_dir.parent_path().parent_path().parent_path();
    auto       script    = repo_root / "scripts" / "video_download.py";
    std::error_code ec;
    return std::filesystem::exists(script, ec) ? script : std::filesystem::path{};
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

VideoDownloader::VideoDownloader()
    : m_cache_dir{}
    , m_mutex{}
    , m_cv{}
    , m_queue{}
    , m_completed{}
    , m_inflight{}
    , m_progress_url{}
    , m_progress_percent{-1.0}
    , m_progress_total{0}
    , m_current_pid{-1}
    , m_worker{} {}

VideoDownloader::~VideoDownloader() {
    shutdown();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void VideoDownloader::set_cache_dir(const std::filesystem::path &dir) {
    m_cache_dir = dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    APP_DEBUG_LOG("[VideoDownloader] cache dir: {}", dir.string());
    start_worker_thread();
}

void VideoDownloader::shutdown() {
    stop_worker_thread();
}

void VideoDownloader::start_worker_thread() {
    if (m_worker)
        return;

    // KillOnly: a hung download is killed via request_stop (the download loop polls
    // the stop_token and quits mpv), and the worker stays unwatched until restarted.
    ManagedThread::Config cfg;
    cfg.name    = "VideoDownloader";
    cfg.timeout = std::chrono::milliseconds(5000);
    cfg.policy  = ThreadOverwatch::RecoveryPolicy::KillOnly;
    m_worker    = std::make_unique<ManagedThread>(
        cfg, [this](const std::stop_token &st, ManagedThread &self) { worker_iteration(st, self); });
}

void VideoDownloader::stop_worker_thread() {
    if (!m_worker)
        return;

    m_worker->request_stop();
    m_cv.notify_all();

    // Interrupt any in-flight yt-dlp so the join returns promptly instead of
    // waiting out a long download (the download loop also polls the stop_token).
    if (const pid_t pid = m_current_pid.load(std::memory_order_acquire); pid > 0)
        ::kill(pid, SIGTERM);

    m_worker.reset(); // ManagedThread destructor joins
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::optional<std::filesystem::path>
VideoDownloader::get_or_enqueue(const std::string &url) {
    if (m_cache_dir.empty())
        return std::nullopt;

    const auto path = cache_path_for(url);

    // Return cached file immediately if it exists and is non-empty.
    std::error_code ec;
    if (std::filesystem::exists(path, ec) &&
        std::filesystem::file_size(path, ec) > 0) {
        APP_DEBUG_LOG("[VideoDownloader] cache hit: {}", path.string());
        return std::filesystem::absolute(path, ec);
    }

    // Enqueue if not already in-flight.
    std::lock_guard lock{m_mutex};
    const bool already =
        std::any_of(m_inflight.begin(), m_inflight.end(),
                    [&url](const std::string &u) { return u == url; });
    if (!already) {
        APP_DEBUG_LOG("[VideoDownloader] enqueue: {}", url);
        m_inflight.push_back(url);
        m_queue.push_back(Job{url, path});
        m_cv.notify_one();
    }
    return std::nullopt;
}

std::vector<VideoDownloader::Result> VideoDownloader::take_completed() {
    std::lock_guard lock{m_mutex};
    return std::exchange(m_completed, {});
}

uint64_t VideoDownloader::bytes_inflight(const std::string &url) const {
    std::lock_guard lock{m_mutex};
    const bool in_flight =
        std::any_of(m_inflight.begin(), m_inflight.end(),
                    [&url](const std::string &u) { return u == url; });
    if (!in_flight)
        return 0;

    std::error_code ec;
    const auto sz = std::filesystem::file_size(cache_path_for(url), ec);
    return ec ? 0 : sz;
}

VideoDownloader::Progress VideoDownloader::progress(const std::string &url) const {
    Progress p;

    std::lock_guard lock{m_mutex};
    p.active = std::any_of(m_inflight.begin(), m_inflight.end(),
                           [&url](const std::string &u) { return u == url; });
    if (!p.active)
        return p;

    if (m_progress_url == url) {
        p.percent = m_progress_percent;
        p.total   = m_progress_total;
    }

    std::error_code ec;
    const auto sz = std::filesystem::file_size(cache_path_for(url), ec);
    p.bytes = ec ? 0 : sz;

    // yt-dlp reports one percentage per stream (video, then audio), so a stale
    // total can end up below the bytes already on disk — keep the bar sane.
    if (p.total > 0 && p.bytes > p.total)
        p.total = p.bytes;

    return p;
}

void VideoDownloader::reset_progress(const std::string &url) {
    std::lock_guard lock{m_mutex};
    m_progress_url     = url;
    m_progress_percent = -1.0;
    m_progress_total   = 0;
}

void VideoDownloader::clear_cache() {
    if (m_cache_dir.empty())
        return;

    // Interrupt any active download.
    if (const pid_t pid = m_current_pid.load(std::memory_order_acquire); pid > 0)
        ::kill(pid, SIGTERM);

    {
        std::lock_guard lock{m_mutex};
        m_queue.clear();
        m_inflight.clear();
        m_completed.clear();
        m_progress_url.clear();
        m_progress_percent = -1.0;
        m_progress_total   = 0;
    }

    std::error_code ec;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(
             m_cache_dir,
             std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (entry.is_regular_file(ec))
            std::filesystem::remove(entry.path(), ec);
    }
}

// ---------------------------------------------------------------------------
// Download (runs on worker thread)
// ---------------------------------------------------------------------------

std::string VideoDownloader::ytdl_format() {
    // Quality/codec policy. yt-dlp merges the chosen video+audio into one mp4.
    //   - prefer an mp4-container video ≤1080p plus m4a audio (h264/aac-friendly,
    //     the most broadly decodable result);
    //   - then any ≤1080p mp4 single file; then any ≤1080p; then absolute best.
    // Tighten the first clause to `[vcodec^=avc1]` to force H.264 over AV1/VP9 if
    // the player struggles with newer codecs.
    return "bestvideo[ext=mp4][height<=1080]+bestaudio[ext=m4a]"
           "/best[ext=mp4][height<=1080]"
           "/best[height<=1080]"
           "/best";
}

bool VideoDownloader::download_succeeded(int exit_code,
                                         const std::filesystem::path &target) {
    // Success policy: yt-dlp must have exited cleanly AND left a non-trivial file.
    // The exit code alone is not enough — the previous stream-dump path "succeeded"
    // while writing 0 bytes, so we require the file to actually exist with content.
    if (exit_code != 0)
        return false;

    std::error_code ec;
    const auto size = std::filesystem::file_size(target, ec);
    return !ec && size > 0;
}

// ---------------------------------------------------------------------------
// Progress parsing (runs on worker thread, from run_child's output pump)
// ---------------------------------------------------------------------------

uint64_t VideoDownloader::parse_size_token(std::string_view token) {
    while (!token.empty() && (token.front() == '~' || token.front() == ' '))
        token.remove_prefix(1);
    if (token.empty() || (std::isdigit(static_cast<unsigned char>(token.front())) == 0))
        return 0; // "Unknown", "--:--:--", "N/A", …

    std::size_t used  = 0;
    double      value = 0.0;
    try {
        value = std::stod(std::string(token), &used);
    } catch (const std::exception &) {
        return 0;
    }

    // Both yt-dlp (KiB/MiB/GiB) and curl (k/M/G) use binary multiples.
    double multiplier = 1.0;
    if (used < token.size()) {
        switch (std::toupper(static_cast<unsigned char>(token[used]))) {
        case 'K': multiplier = 1024.0; break;
        case 'M': multiplier = 1024.0 * 1024.0; break;
        case 'G': multiplier = 1024.0 * 1024.0 * 1024.0; break;
        case 'T': multiplier = 1024.0 * 1024.0 * 1024.0 * 1024.0; break;
        default:  break; // plain bytes
        }
    }
    return static_cast<uint64_t>(value * multiplier);
}

void VideoDownloader::parse_progress_line(std::string_view line) {
    // Split into whitespace-separated tokens (the meter rows are short).
    std::array<std::string_view, 16> tok{};
    std::size_t                      count = 0;
    for (std::size_t i = 0; i < line.size() && count < tok.size();) {
        while (i < line.size() && (std::isspace(static_cast<unsigned char>(line[i])) != 0))
            ++i;
        const std::size_t start = i;
        while (i < line.size() && (std::isspace(static_cast<unsigned char>(line[i])) == 0))
            ++i;
        if (i > start)
            tok[count++] = line.substr(start, i - start);
    }
    if (count == 0)
        return;

    double   percent = -1.0;
    uint64_t total   = 0;

    const auto all_digits = [](std::string_view sv) {
        return !sv.empty() && std::all_of(sv.begin(), sv.end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c)) != 0;
        });
    };

    if (tok[0] == "[download]" && count >= 2 && tok[1].back() == '%' &&
        std::isdigit(static_cast<unsigned char>(tok[1].front())) != 0) {
        // yt-dlp --newline: "[download]  45.3% of  123.45MiB at 2.10MiB/s ETA 00:37"
        // A live stream reports an estimate as "of ~ 123.45MiB" (tilde split off).
        percent = std::strtod(std::string(tok[1]).c_str(), nullptr);
        if (count >= 4 && tok[2] == "of")
            total = tok[3] == "~" && count >= 5 ? parse_size_token(tok[4])
                                                : parse_size_token(tok[3]);
    } else if (count >= 8 && all_digits(tok[0]) && all_digits(tok[2]) &&
               parse_size_token(tok[1]) > 0) {
        // curl's meter row: "% Total  % Received % Xferd  Dload Upload  Time…"
        // — column 0 is the percentage of the whole transfer, column 1 its size.
        // The trailing time columns go blank near the end, so don't demand 12.
        percent = std::strtod(std::string(tok[0]).c_str(), nullptr);
        total   = parse_size_token(tok[1]);
    } else {
        return; // not a progress line
    }

    if (percent < 0.0 && total == 0)
        return;

    std::lock_guard lock{m_mutex};
    if (percent >= 0.0)
        m_progress_percent = std::clamp(percent, 0.0, 100.0);
    if (total > 0)
        m_progress_total = total;
}

int VideoDownloader::run_child(const char *const *argv,
                               const std::stop_token &st,
                               ManagedThread &self,
                               std::string &tail) {
    // Pipe captures the child's stdout+stderr so failures are logged (not
    // swallowed) and so the parent has something to poll while keeping the
    // watchdog alive.
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        APP_DEBUG_LOG("[VideoDownloader] pipe() failed: {}", std::strerror(errno));
        return -1;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        APP_DEBUG_LOG("[VideoDownloader] fork() failed: {}", std::strerror(errno));
        return -1;
    }

    if (pid == 0) {
        // Child: redirect stdout+stderr into the pipe, then exec.
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        ::execvp(argv[0], const_cast<char *const *>(argv));
        _exit(127); // exec failed — tool not on PATH
    }

    // Parent.
    ::close(pipefd[1]);
    m_current_pid.store(pid, std::memory_order_release);

    std::string lines; // partial-line buffer for the progress parser
    bool        termed = false;
    for (;;) {
        if (st.stop_requested() && !termed) {
            ::kill(pid, SIGTERM);
            termed = true;
        }

        pollfd pfd{pipefd[0], POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 300); // 300 ms cadence: services stop + heartbeat
        self.heartbeat();

        if (pr > 0 && (pfd.revents & POLLIN) != 0) {
            std::array<char, 4096> buf{};
            const ssize_t n = ::read(pipefd[0], buf.data(), buf.size());
            if (n > 0) {
                tail.append(buf.data(), static_cast<std::size_t>(n));
                if (tail.size() > 8192)
                    tail.erase(0, tail.size() - 8192);

                // Feed complete lines to the progress parser. Both terminators
                // matter: yt-dlp --newline emits '\n', curl's meter redraws the
                // same row with '\r'.
                lines.append(buf.data(), static_cast<std::size_t>(n));
                for (std::size_t end; (end = lines.find_first_of("\r\n")) != std::string::npos;) {
                    parse_progress_line(std::string_view{lines}.substr(0, end));
                    lines.erase(0, end + 1);
                }
                if (lines.size() > 1024)
                    lines.clear(); // no line break in sight — not progress output
            }
        }

        int         status = 0;
        const pid_t reaped = ::waitpid(pid, &status, WNOHANG);
        if (reaped != pid)
            continue; // still running

        m_current_pid.store(-1, std::memory_order_release);
        ::close(pipefd[0]);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
}

bool VideoDownloader::download(const std::string &url,
                               const std::filesystem::path &target,
                               const std::stop_token &st,
                               ManagedThread &self) {
    std::error_code ec;
    std::filesystem::remove(target, ec); // clear any stale 0-byte remnant
    reset_progress(url);

    const std::string fmt = ytdl_format();
    const std::string out = target.string();

    // --- Attempt 1: yt-dlp ---------------------------------------------------
    // yt-dlp downloads AND muxes separate DASH/HLS video+audio tracks into one
    // playable file — something mpv's --stream-dump cannot do: ytdl_hook hands
    // mpv an edl:// pseudo-stream (no raw bytes), so stream-dump silently wrote
    // 0-byte files. See systematic-debugging trace 2026-06-20.
    //
    // --compat-options allow-unsafe-ext disables yt-dlp's unsafe-extension guard
    // (GHSA-79w7-vh3h-8g4j), which otherwise aborts when a CDN streams video via
    // a script path — e.g. .../remote_control.php → yt-dlp derives ext='php' and
    // refuses the only format. Our explicit -o pins the on-disk name to
    // <hash>.mp4 regardless (so the flag's usual risk is neutralised), and
    // --remux-video mp4 guarantees an mp4 container. See trace 2026-06-21.
    APP_DEBUG_LOG("[VideoDownloader] download start (yt-dlp): {} -> {}", url, out);
    {
        const char *argv[] = {"yt-dlp", "--no-warnings", "--no-playlist",
                              // --newline + --progress: one progress line per
                              // update on a pipe, which parse_progress_line reads.
                              "--newline", "--progress",
                              "--compat-options", "allow-unsafe-ext",
                              "--merge-output-format", "mp4",
                              "--remux-video", "mp4",
                              "-f", fmt.c_str(),
                              "-o", out.c_str(),
                              "--", url.c_str(), nullptr};
        std::string tail;
        const int   code = run_child(argv, st, self, tail);
        if (download_succeeded(code, target) && !st.stop_requested()) {
            APP_DEBUG_LOG("[VideoDownloader] download OK (yt-dlp): {}", out);
            return true;
        }
        std::filesystem::remove(target, ec);
        APP_DEBUG_LOG("[VideoDownloader] yt-dlp FAILED (exit={}): {}\n{}", code, url, tail);
    }

    if (st.stop_requested())
        return false;

    // --- Attempt 2: vendored yt-dlp via python (newer extractors) -----------
    // The distro's yt-dlp binary can lag the vendored external/yt-dlp checkout,
    // which may carry extractor fixes the binary lacks. scripts/video_download.py
    // imports that newer yt_dlp (with the unsafe-extension guard disabled) and
    // writes the canonical <hash>.mp4. Skipped if the script can't be located.
    if (const auto script = python_fallback_script(); !script.empty()) {
        APP_DEBUG_LOG("[VideoDownloader] yt-dlp failed; python fallback: {} -> {}", url, out);
        reset_progress(url);
        const std::string py = script.string();
        const char *argv[] = {"python3", py.c_str(),
                              url.c_str(), out.c_str(), fmt.c_str(), nullptr};
        std::string tail;
        const int   code = run_child(argv, st, self, tail);
        if (download_succeeded(code, target) && !st.stop_requested()) {
            APP_DEBUG_LOG("[VideoDownloader] download OK (python): {}", out);
            return true;
        }
        std::filesystem::remove(target, ec);
        APP_DEBUG_LOG("[VideoDownloader] python fallback FAILED (exit={}): {}\n{}", code,
                      url, tail);
    } else {
        APP_DEBUG_LOG("[VideoDownloader] python fallback script not found; skipping");
    }

    if (st.stop_requested())
        return false;

    // --- Attempt 3: curl direct download (last resort) ----------------------
    // For authenticated direct-media links (get_file / CDN redirects) with no
    // real extractor, curl follows the redirect and writes the bytes regardless
    // of the URL path. No muxing — but these links are always single
    // self-contained files, so that's fine. --fail rejects HTTP error pages; a
    // browser User-Agent satisfies CDNs that gate non-browser clients.
    APP_DEBUG_LOG("[VideoDownloader] curl fallback: {} -> {}", url, out);
    reset_progress(url);
    {
        // No --silent here: curl's column meter on stderr is the progress
        // source parse_progress_line reads (--show-error still reports failures).
        const char *argv[] = {"curl", "-L", "--fail", "--show-error",
                              "--connect-timeout", "30",
                              "-A", kFallbackUserAgent,
                              "-o", out.c_str(),
                              "--", url.c_str(), nullptr};
        std::string tail;
        const int   code = run_child(argv, st, self, tail);
        if (download_succeeded(code, target) && !st.stop_requested()) {
            APP_DEBUG_LOG("[VideoDownloader] download OK (curl): {}", out);
            return true;
        }
        std::filesystem::remove(target, ec);
        APP_DEBUG_LOG("[VideoDownloader] curl FAILED (exit={}): {}\n{}", code, url, tail);
    }

    return false;
}

// ---------------------------------------------------------------------------
// Worker loop
// ---------------------------------------------------------------------------

void VideoDownloader::worker_iteration(const std::stop_token &st, ManagedThread &self) {
    Job job;
    {
        std::unique_lock lock{m_mutex};
        m_cv.wait_for(lock, std::chrono::milliseconds(500), [this, &st] {
            return st.stop_requested() || !m_queue.empty();
        });

        // Heartbeat after waking — wait_for can hold for up to 500 ms.
        self.heartbeat();

        if (st.stop_requested() || m_queue.empty())
            return;

        job = std::move(m_queue.front());
        m_queue.erase(m_queue.begin());
    }

    APP_DEBUG_LOG("[VideoDownloader] worker: picked up job: {}", job.url);
    const bool ok = download(job.url, job.target, st, self);

    reset_progress({}); // no job in flight — progress() falls back to file size

    std::lock_guard lock{m_mutex};
    m_inflight.erase(std::remove(m_inflight.begin(), m_inflight.end(), job.url), m_inflight.end());
    m_completed.push_back({job.url, ok ? job.target : std::filesystem::path{}, ok});
    APP_DEBUG_LOG("[VideoDownloader] worker: completed (ok={}) {}", ok, job.url);
}