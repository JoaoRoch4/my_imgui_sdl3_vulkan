#pragma once
#include "pch.hpp"

// Live ImGui table of every OS thread in the process (enumerated from
// /proc/self/task), left-joined against ThreadRegistry so ManagedThread-tracked
// threads show rich columns (policy, iterations, restarts, status) and the rest
// show name + OS run-state only. Toggled from the app debug menu.
class ThreadReflectionPanel {
public:
    void draw(bool *open);

private:
    // One OS thread, as read from /proc/self/task/<tid>/.
    struct OsThread {
        pid_t       tid   = 0;
        std::string comm;          // pthread name (<= 15 chars)
        char        state = '?';   // R/S/D/Z/T/... from /proc/.../stat
    };

    void refresh_os_threads(); // re-read /proc/self/task (throttled)

    std::vector<OsThread>                 m_os_cache;
    std::chrono::steady_clock::time_point m_last_refresh{};
    bool                                  m_show_unmanaged = true;
};
