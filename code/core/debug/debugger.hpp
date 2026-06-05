#pragma once
#include "pch.hpp"

// Lightweight, header-only debugger helpers (Linux). Header-only so any
// translation unit can use them without a link dependency.
namespace appdebug {

// True when this process is currently traced (TracerPid != 0 in
// /proc/self/status) — i.e. running under a debugger such as lldb/gdb. Re-read on
// every call (calls are rare) so attaching a debugger later is still detected.
[[nodiscard]] inline bool is_debugger_present() noexcept
{
    constexpr std::string_view tracer_key{"TracerPid:"};

    std::ifstream status{"/proc/self/status"};
    if (!status.is_open())
        return false;

    for (std::string line; std::getline(status, line);) {
        std::string_view view{line};
        if (!view.starts_with(tracer_key))
            continue;

        view.remove_prefix(tracer_key.size());
        const std::size_t first = view.find_first_not_of(" \t");
        const std::size_t last  = view.find_last_not_of(" \t");
        if (first == std::string_view::npos)
            return false;
        view = view.substr(first, last - first + 1);
        return view != "0";
    }
    return false;
}

// Trap into the attached debugger (breakpoint). WARNING: only call when a
// debugger is attached — with none, the default SIGTRAP disposition terminates
// the process. Guard every call with is_debugger_present().
inline void debug_break() noexcept
{
#if defined(__has_builtin)
#if __has_builtin(__builtin_debugtrap)
    __builtin_debugtrap();
    return;
#endif
#endif
    std::raise(SIGTRAP);
}

} // namespace appdebug
