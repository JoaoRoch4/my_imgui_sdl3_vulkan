#include "history_context_menu.hpp"



std::optional<std::string> HistoryContextMenu::draw_for_item(const std::string &source)
{
    if (!ImGui::BeginPopupContextItem())
        return std::nullopt;

    // Show a short preview of the source as a non-interactive header.
    const std::string preview = source.size() > 64
        ? source.substr(0, 61) + "..."
        : source;
    ImGui::TextDisabled("%s", preview.c_str());
    ImGui::Separator();

    std::optional<std::string> result;
    if (ImGui::MenuItem("Remove from History"))
        result = source;

    ImGui::EndPopup();
    return result;
}
