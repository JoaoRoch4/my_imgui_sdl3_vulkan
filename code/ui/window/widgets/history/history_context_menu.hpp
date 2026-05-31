#pragma once


/// Renders a right-click context menu for a single history entry.
///
/// Usage — call draw_for_item() immediately after the ImGui widget
/// (Selectable, MenuItem, …) that represents the history entry.
/// The method hooks onto the last widget via BeginPopupContextItem.
///
/// Returns the source string of the entry to remove when the user
/// chooses "Remove from History", or std::nullopt otherwise.
class HistoryContextMenu {
public:
    /// Attach a context menu to the last rendered ImGui item.
    /// @param source  The source (path or URL) stored in the history entry.
    /// @return        The source to erase if the user confirmed removal, else nullopt.
    static std::optional<std::string> draw_for_item(const std::string &source);
};
