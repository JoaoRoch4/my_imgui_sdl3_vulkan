#include "pch.hpp"
#include "thread_reflection_panel.hpp"
#include "thread_registry.hpp"

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

void ThreadReflectionPanel::draw(bool *open)
{
    if (open != nullptr && !*open)
        return;

    if (!ImGui::Begin("Threads", open)) {
        ImGui::End();
        return;
    }

    auto snap = ThreadRegistry::instance().snapshot();
    std::ranges::sort(snap, [](const ThreadInfo &a, const ThreadInfo &b) { return a.name < b.name; });

    const auto now = std::chrono::steady_clock::now();
    ImGui::Text("Managed threads: %zu", snap.size());

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

        for (const auto &t : snap) {
            ImGui::TableNextRow();
            if (t.state == ThreadState::Failed)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(120, 30, 30, 160));
            else if (t.state == ThreadState::Restarting)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(120, 90, 20, 140));

            const auto hb_age =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - t.last_heartbeat).count();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(t.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", static_cast<int>(t.tid));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(state_name(t.state));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(policy_name(t.policy));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(t.iterations));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(t.restart_count));
            ImGui::TableNextColumn();
            ImGui::Text("%lld", static_cast<long long>(hb_age));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(t.overwatch_watch_id));
            ImGui::TableNextColumn();
            if (t.state == ThreadState::Failed && !t.failed_reason.empty())
                ImGui::TextUnformatted(t.failed_reason.c_str());
            else
                ImGui::TextUnformatted(join_status(t.status).c_str());
        }
        ImGui::EndTable();
    }

    ImGui::End();
}
