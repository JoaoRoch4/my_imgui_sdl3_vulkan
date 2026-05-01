#include "window_state_toml.hpp"

#include <rfl/toml/write.hpp>
#include <rfl/toml/save.hpp>
#include <rfl/toml.hpp>

#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

struct CommentRule
{
    const char* prefix;
    const char* comment;
};

std::string LTrim(const std::string& text)
{
    size_t index = 0;
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text.at(index))))
        ++index;
    return text.substr(index);
}

std::string Trim(const std::string& text)
{
    if (text.empty())
        return text;
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text.at(start))))
        ++start;
    if (start == text.size())
        return {};
    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text.at(end - 1))))
        --end;
    return text.substr(start, end - start);
}

bool IsHeaderLine(const std::string& trimmed)
{
    return (trimmed.starts_with("[") && trimmed.ends_with("]"));
}

std::string HeaderName(const std::string& trimmed)
{
    if (!IsHeaderLine(trimmed))
        return {};
    if (trimmed.starts_with("[[") && trimmed.ends_with("]]"))
        return trimmed.substr(2, trimmed.size() - 4);
    return trimmed.substr(1, trimmed.size() - 2);
}

bool IsKeyValueLine(const std::string& trimmed)
{
    if (trimmed.empty() || trimmed.starts_with("#"))
        return false;
    return trimmed.find('=') != std::string::npos;
}

std::string KeyName(const std::string& trimmed)
{
    const size_t equals_pos = trimmed.find('=');
    if (equals_pos == std::string::npos)
        return {};
    return Trim(trimmed.substr(0, equals_pos));
}

std::string FieldCommentFor(const std::string& section_name, const std::string& key)
{
    if (key == "show_demo_window")
        return "# Show Dear ImGui demo window.";
    if (key == "show_another_window")
        return "# Show the 'Another Window' sample panel.";
    if (key == "vsync")
        return "# Enable vertical synchronization (FIFO). Disable for uncapped frame rate (MAILBOX/IMMEDIATE).";

    if (section_name == "clear_color" || section_name == "style.colors")
    {
        if (key == "r") return "# Red (0-255).";
        if (key == "g") return "# Green (0-255).";
        if (key == "b") return "# Blue (0-255).";
        if (key == "a") return "# Alpha (0-255).";
    }

    const bool is_window_rect = (section_name == "hello_world_window" || section_name == "another_window");
    if (is_window_rect)
    {
        if (key == "x")     return "# Window left position in pixels.";
        if (key == "y")     return "# Window top position in pixels.";
        if (key == "w")     return "# Window width in pixels.";
        if (key == "h")     return "# Window height in pixels.";
        if (key == "valid") return "# True if this saved rectangle should be applied.";
    }

    if (key == "x") return "# X component.";
    if (key == "y") return "# Y component.";
    if (key == "z") return "# Z component.";
    if (key == "w") return "# W component.";

    return "# Saved value for '" + key + "'.";
}

void AnnotateWindowStateToml(const std::filesystem::path& file_path)
{
    std::ifstream input(file_path);
    if (!input.is_open())
        return;

    std::vector<std::string> lines;
    std::string              line;
    while (std::getline(input, line))
        lines.push_back(line);
    input.close();

    const std::array<CommentRule, 7> rules {
        CommentRule { "show_demo_window =",      "# Window toggles persisted between runs." },
        CommentRule { "[clear_color]",            "# Clear color used as the Vulkan background clear value." },
        CommentRule { "[hello_world_window]",     "# Window rectangle fields: x/y = top-left, w/h = size, valid = apply saved rect." },
        CommentRule { "[another_window]",         "# Another Window rectangle." },
        CommentRule { "[style_editor_window]",    "# Style Editor window rectangle." },
        CommentRule { "[style]",                  "# Persisted ImGui style (preset + full color/sizing overrides)." },
        CommentRule { "[[image_history]]",         "# History of opened images and URLs (newest first, max 100 entries)." },
    };

    std::array<bool, rules.size()> emitted_rule_comment {};
    std::string                    current_section;
    int                            style_colors_index = 0;

    std::vector<std::string> annotated;
    annotated.reserve(lines.size() * 2 + 8);
    annotated.push_back("# SDL3+Vulkan ImGui persisted state (auto-generated). Manual edits are allowed.");

    for (const std::string& current : lines)
    {
        const std::string trimmed = LTrim(current);

        if (IsHeaderLine(trimmed))
        {
            current_section = HeaderName(trimmed);
            if (trimmed == "[[style.colors]]" && style_colors_index < ImGuiCol_COUNT)
            {
                std::string color_comment = "# ImGuiCol_";
                color_comment += ImGui::GetStyleColorName(style_colors_index++);
                annotated.push_back(color_comment);
            }
        }

        for (size_t rule_index = 0; rule_index < rules.size(); ++rule_index)
        {
            const CommentRule& rule = rules.at(rule_index);
            if (trimmed.starts_with(rule.prefix))
            {
                if (!emitted_rule_comment.at(rule_index))
                {
                    annotated.emplace_back(rule.comment);
                    emitted_rule_comment.at(rule_index) = true;
                }
                break;
            }
        }

        if (IsKeyValueLine(trimmed))
        {
            const std::string key           = KeyName(trimmed);
            const std::string field_comment = FieldCommentFor(current_section, key);
            if (!field_comment.empty())
                annotated.push_back(field_comment);
        }

        annotated.push_back(current);
    }

    std::ofstream output(file_path, std::ios::trunc);
    if (!output.is_open())
        return;
    for (const std::string& annotated_line : annotated)
        output << annotated_line << '\n';
}

} // namespace

bool LoadWindowStateToml(const std::filesystem::path& file_path, WindowStateToml& state)
{
    const auto result = rfl::toml::load<WindowStateToml>(file_path.string());
    if (!result)
        return false;
    state = result.value();
    return true;
}

bool SaveWindowStateToml(const std::filesystem::path& file_path, const WindowStateToml& state)
{
    std::filesystem::create_directories(file_path.parent_path());
    rfl::toml::save(file_path.string(), state);
    AnnotateWindowStateToml(file_path);
    return true;
}
