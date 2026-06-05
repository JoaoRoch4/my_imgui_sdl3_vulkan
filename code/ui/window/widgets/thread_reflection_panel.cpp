#include "pch.hpp"
#include "thread_reflection_panel.hpp"
#include "thread_registry.hpp"
#include "core/debug/debugger.hpp"

namespace {

const char *state_name(ThreadState s)
{
    switch (s) {
    case ThreadState::Starting:   return "Starting";
    case ThreadState::Running:    return "Running";
    case ThreadState::Stopping:   return "Stopping";
    case ThreadState::Stopped:    return "Stopped";
    case ThreadState::Restarting: return "Restarting";
    case ThreadState::Failed:     return "Failed";
    }
    return "?";
}

const char *policy_name(ThreadOverwatch::RecoveryPolicy p)
{
    return p == ThreadOverwatch::RecoveryPolicy::KillOnly ? "KillOnly" : "Restart";
}

// Map a Linux /proc/.../stat run-state char to a readable word.
const char *os_state_name(char s)
{
    switch (s) {
    case 'R': return "Running";
    case 'S': return "Sleeping";
    case 'D': return "Disk";     // uninterruptible sleep
    case 'Z': return "Zombie";
    case 'T': return "Stopped";
    case 't': return "Tracing";
    case 'I': return "Idle";
    case 'W': return "Paging";
    case 'X':
    case 'x': return "Dead";
    default:  return "?";
    }
}

std::string join_status(std::span<const std::pair<std::string, std::string>> kv)
{
    std::string out;
    for (const auto &[k, v] : kv) {
        if (!out.empty())
            out += "  ";
        out += std::format("{}={}", k, v);
    }
    return out;
}

} // namespace

void ThreadReflectionPanel::refresh_os_threads()
{
    m_os_cache.clear();

    std::error_code ec;
    const std::filesystem::directory_iterator dir("/proc/self/task", ec);
    if (ec)
        return; // non-Linux or /proc unavailable — table will show managed-only rows

    for (const auto &entry : dir) {
        const std::string tid_str = entry.path().filename().string();

        pid_t      tid = 0;
        const auto res = std::from_chars(tid_str.data(), tid_str.data() + tid_str.size(), tid);
        if (res.ec != std::errc{})
            continue;

        OsThread t{};
        t.tid = tid;

        // Thread name (15 chars max). One line, trailing newline stripped by getline.
        if (std::ifstream comm_file(entry.path() / "comm"); comm_file)
            std::getline(comm_file, t.comm);

        // Run state: the single char after the last ')' in stat. Parsing after the
        // last ')' is robust to comm values that contain spaces or parentheses.
        if (std::ifstream stat_file(entry.path() / "stat"); stat_file) {
            std::string stat;
            std::getline(stat_file, stat);
            if (const auto rp = stat.rfind(')'); rp != std::string::npos && rp + 2 < stat.size())
                t.state = stat[rp + 2];
        }

        m_os_cache.push_back(std::move(t));
    }
}

void ThreadReflectionPanel::draw(bool *open)
{
    if (open != nullptr && !*open)
        return;

    if (!ImGui::Begin("Threads", open)) {
        ImGui::End();
        return;
    }

    // /proc is filesystem I/O — refresh at most ~2x/second; managed columns below
    // still update every frame from the in-memory registry snapshot.
    const auto now = std::chrono::steady_clock::now();
    if (m_os_cache.empty() || now - m_last_refresh > std::chrono::milliseconds(500)) {
        refresh_os_threads();
        m_last_refresh = now;
    }

    const auto snap = ThreadRegistry::instance().snapshot();

    // tid -> managed info (only entries that have published a tid).
    std::unordered_map<pid_t, const ThreadInfo *> managed;
    managed.reserve(snap.size());
    for (const auto &t : snap)
        if (t.tid != 0)
            managed.emplace(t.tid, &t);

    // Unified row: an OS thread, optionally joined to its ManagedThread record.
    struct Row {
        std::string       name;
        pid_t             tid      = 0;
        const ThreadInfo *mgd      = nullptr; // nullptr → unmanaged
        char              os_state = '?';
        bool              os_alive = true;    // false → registry entry with no live OS thread
    };

    std::vector<Row> rows;
    std::unordered_set<pid_t> os_tids;
    rows.reserve(m_os_cache.size() + snap.size());

    std::size_t managed_count = 0;
    for (const auto &o : m_os_cache) {
        os_tids.insert(o.tid);
        const ThreadInfo *m = nullptr;
        if (const auto it = managed.find(o.tid); it != managed.end()) {
            m = it->second;
            ++managed_count;
        }
        if (m == nullptr && !m_show_unmanaged)
            continue;
        rows.push_back({m != nullptr ? m->name : o.comm, o.tid, m, o.state, true});
    }

    // Managed threads with no live OS thread yet/anymore (Starting tid==0, or Stopped).
    for (const auto &t : snap) {
        if (t.tid != 0 && os_tids.contains(t.tid))
            continue;
        rows.push_back({t.name, t.tid, &t, '?', false});
    }

    std::ranges::sort(rows, [](const Row &a, const Row &b) {
        if ((a.mgd != nullptr) != (b.mgd != nullptr))
            return a.mgd != nullptr; // managed first
        return a.name < b.name;
    });

    ImGui::Checkbox("Show unmanaged", &m_show_unmanaged);
    ImGui::SameLine();
    ImGui::Text("| process threads: %zu   managed: %zu", m_os_cache.size(), managed_count);

    // Break into the debugger (only when one is attached — otherwise SIGTRAP would
    // terminate the process). Re-checked each frame so attaching lldb mid-run works.
    const bool debugger = appdebug::is_debugger_present();
    ImGui::SameLine();
    ImGui::BeginDisabled(!debugger);
    if (ImGui::Button("Break into debugger") && debugger)
        appdebug::debug_break();
    ImGui::EndDisabled();
    if (!debugger && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Run under a debugger (lldb) to enable");

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("threads_table", 9, flags)) {
        ImGui::TableSetupColumn("name");
        ImGui::TableSetupColumn("tid");
        ImGui::TableSetupColumn("state");
        ImGui::TableSetupColumn("policy");
        ImGui::TableSetupColumn("iters");
        ImGui::TableSetupColumn("restarts");
        ImGui::TableSetupColumn("HB age (ms)");
        ImGui::TableSetupColumn("watch");
        ImGui::TableSetupColumn("status");
        ImGui::TableHeadersRow();

        for (const auto &r : rows) {
            const ThreadInfo *m = r.mgd;
            ImGui::TableNextRow();

            if (m != nullptr && m->state == ThreadState::Failed)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(120, 30, 30, 160));
            else if (m != nullptr && m->state == ThreadState::Restarting)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(120, 90, 20, 140));

            // Unmanaged rows are dimmed to keep managed threads visually primary.
            const bool dim = (m == nullptr);
            if (dim)
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", static_cast<int>(r.tid));
            ImGui::TableNextColumn();
            if (m != nullptr)
                ImGui::TextUnformatted(state_name(m->state));
            else
                ImGui::TextUnformatted(r.os_alive ? os_state_name(r.os_state) : "gone");

            if (m != nullptr) {
                const auto hb_age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        now - m->last_heartbeat)
                                        .count();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(policy_name(m->policy));
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(m->iterations));
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(m->restart_count));
                ImGui::TableNextColumn();
                ImGui::Text("%lld", static_cast<long long>(hb_age));
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(m->overwatch_watch_id));
                ImGui::TableNextColumn();
                if (m->state == ThreadState::Failed && !m->failed_reason.empty())
                    ImGui::TextUnformatted(m->failed_reason.c_str());
                else
                    ImGui::TextUnformatted(join_status(m->status).c_str());
            } else {
                for (int c = 0; c < 5; ++c) {
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted("\xe2\x80\x94"); // em dash
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("(unmanaged)");
            }

            if (dim)
                ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}
