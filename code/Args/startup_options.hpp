#pragma once

#include "pch.hpp"

class Args;

/// Semantic result of parsing the command line.
///
/// `Args` is a dumb token-matcher (`hasArg("--x")`). `StartupOptions` is the
/// resolved *intent* that the rest of the app reads — downstream code never
/// touches argv directly. Built once at startup by resolve_startup_options()
/// and handed to every `App` instance (so the flags survive a reopen).
///
/// These override values loaded from window_state.toml, but as a *session-only*
/// override: App snapshots the original TOML value and restores it before save,
/// so a one-off flag never rewrites your saved preferences on disk.
struct StartupOptions {
	/// --monitor-thread : force the live Threads reflection panel open on launch
	/// (and log a line confirming the ThreadOverwatch monitor is active), even
	/// in release builds where it is otherwise hidden.
	bool monitor_thread = false;

	/// --file-browser / --no-file-browser : force the file explorer window open
	/// or closed. std::nullopt means "no override — use the TOML value".
	std::optional<bool> file_browser;

	/// --no-video : do not load or play any video this session (images still work).
	bool disable_video = false;

	/// --no-media : do not load ANY media this session (implies --no-video).
	bool disable_media = false;
};

/// Translate parsed command-line tokens into resolved StartupOptions.
[[nodiscard]] StartupOptions resolve_startup_options(const Args &args);

/// Human-readable, one-line-per-effect summary of what the resolved options did
/// (e.g. "--no-video: video disabled this session"). Used as the single source
/// of truth for both the stdout log and the in-app console feedback, so the two
/// never drift. Returns an EMPTY vector when no flag took effect, so the caller
/// stays silent (no print, no console line, no auto-open).
[[nodiscard]] std::vector<std::string> describe_startup_options(const StartupOptions &opts);

/// Resolve the tri-state file-browser override from the two mutually-opposed
/// flags. Returns:
///   - std::nullopt        when neither flag is present (fall back to TOML)
///   - std::optional{true} to force the browser open
///   - std::optional{false} to force the browser closed
///
/// YOUR DECISION: what should happen when BOTH --file-browser and
/// --no-file-browser are passed? See startup_options.cpp.
[[nodiscard]] std::optional<bool> resolve_file_browser_override(bool force_open, bool force_closed);
