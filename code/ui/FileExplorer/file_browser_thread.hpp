#pragma once

#include "pch.hpp"



class ManagedThread;

/// One directory entry produced by a scan. Pure data, no ImGui dependency, so
/// it can be filled on the scanner thread and consumed by the UI thread.
struct FileRecord {
		bool                            isDir = false;
		std::filesystem::path           name;
		std::string                     showName;
		std::filesystem::path           extension;
		std::uintmax_t                  size          = 0;
		std::filesystem::file_time_type lastWriteTime = {};
		std::int64_t creationTime = 0; ///< birth time (statx STATX_BTIME), ns since epoch; 0 if
									   ///< unknown
		std::vector<std::string> tags; ///< parsed user.xdg.tags xattr; empty for dirs

		/// Thumbnail fast-path, computed once per scan (see FileBrowser::PollScan) so the
		/// per-frame render loop never re-normalizes the path or re-parses the extension.
		/// Empty key => not thumbnailable (skip the thumbnail engine entirely for this row).
		std::string thumbKey;
		bool        isVideoThumb = false;
		bool        isGifThumb   = false; ///< .gif: eligible for inline animated playback

		/// Source media dimensions in pixels. Filled at scan time via stbi_info (cheap
		/// header-only read) for IMAGES; videos leave these at 0 because the equivalent
		/// libavformat probe is too costly to run on every file during a fresh scan. The
		/// masonry view consumes this to size cells by native aspect; everything else is
		/// indifferent. Both 0 = "unknown, assume 16:9 for layout".
		Uint64 source_w = 0;
		Uint64 source_h = 0;
};

/// Background directory scanner for the file browser.
///
/// A single persistent std::jthread services scan requests. Navigation is
/// latest-wins: each request() bumps a generation counter; the worker abandons
/// any in-flight scan whose generation is no longer current, so the UI thread
/// never blocks or joins while browsing. The active scan is registered with
/// ThreadOverwatch (per-scan, KillOnly) and heartbeats every directory entry.
class FileBrowserScanner {
	public:

		/// Finished scan handed back to the UI thread.
		struct Result {
				std::uint64_t           generation = 0;
				std::vector<FileRecord> records;
				bool                    ok = true;
				std::string             status; ///< error text on failure, empty otherwise
		};

		FileBrowserScanner();
		~FileBrowserScanner();

		FileBrowserScanner(FileBrowserScanner const&)            = delete;
		FileBrowserScanner& operator=(FileBrowserScanner const&) = delete;

		/// Queue an async scan of `dir`, cancelling any in-flight/pending scan
		/// (latest-wins). Returns the generation id assigned to this request.
		std::uint64_t request(std::filesystem::path dir, bool skip_errors);

		/// Non-blocking. If the newest scan has finished, moves its result into
		/// `out` and returns true. Superseded results are dropped.
		bool poll(Result& out);

		/// Stop the worker and release any active overwatch registration.
		void shutdown();

	private:

		struct Request {
				std::filesystem::path dir;
				bool                  skip_errors = false;
				std::uint64_t         generation  = 0;
		};

		void worker_iteration(std::stop_token const& stoken, ManagedThread& self);
		std::vector<FileRecord> scan(Request const& job, std::stop_token const& stoken,
			ManagedThread& self, bool& ok, std::string& status);

		std::mutex                     m_mutex;
		std::condition_variable_any    m_cv;
		std::optional<Request>         m_pending;
		Result                         m_result;
		bool                           m_has_result = false;
		std::atomic<std::uint64_t>     m_latest_gen {0};
		std::unique_ptr<ManagedThread> m_worker;
};
