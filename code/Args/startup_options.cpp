#include "pch.hpp"

#include "startup_options.hpp"

#include "Args.hpp"

StartupOptions resolve_startup_options(const Args &args) {
	StartupOptions opts;

	opts.monitor_thread = args.hasArg("--monitor-thread");
	opts.disable_video  = args.hasArg("--no-video");
	opts.disable_media  = args.hasArg("--no-media");

	opts.file_browser =
		resolve_file_browser_override(args.hasArg("--file-browser"), args.hasArg("--no-file-browser"));

	return opts;
}

std::vector<std::string> describe_startup_options(const StartupOptions &opts) {
	std::vector<std::string> lines;

	if (opts.monitor_thread)
		lines.emplace_back("--monitor-thread: Threads panel forced open; ThreadOverwatch monitor active");

	if (opts.file_browser)
		lines.emplace_back(*opts.file_browser
			? "--file-browser: file explorer forced OPEN (session-only override)"
			: "--no-file-browser: file explorer forced CLOSED (session-only override)");

	// --no-media implies --no-video, so report the broader flag and skip the
	// redundant video line when both effects are in force.
	if (opts.disable_media)
		lines.emplace_back("--no-media: all media loading disabled this session");
	else if (opts.disable_video)
		lines.emplace_back("--no-video: video loading/playback disabled this session");

	// Deliberately empty when no flag took effect: the caller stays silent and
	// does not auto-open the console.
	return lines;
}

// ───────────────────────────────────────────────────────────────────────────
// TODO(you): implement this — it is the one real decision in this feature.
//
// `--file-browser` and `--no-file-browser` are mutually opposed. Map the two
// booleans to a tri-state std::optional<bool>:
//
//   neither flag        -> std::nullopt        (no override; App keeps the TOML value)
//   only --file-browser -> std::optional{true}  (force the explorer OPEN)
//   only --no-file-...   -> std::optional{false} (force the explorer CLOSED)
//   BOTH flags          -> ??? your call ???
//
// The only genuine ambiguity is the last line: when a user passes BOTH, which
// wins? Common conventions:
//   * "explicit-off wins"  — safest: a --no-* flag always disables (return false)
//   * "last one wins"      — needs argv order, which hasArg() does not give you
//   * treat as user error  — ignore both / return nullopt
//
// Pick the policy you want and return accordingly (≈5 lines).
std::optional<bool> resolve_file_browser_override(bool force_open, bool force_closed) {
	(void)force_open;
	(void)force_closed;
	return std::nullopt; // <-- replace with your resolution
}
