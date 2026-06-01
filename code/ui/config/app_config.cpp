#include "pch.hpp"

#include "app_config.hpp"
ImVec4 app_config::clear_color() const { return ImVec4(clear_r, clear_g, clear_b, clear_a); }

bool app_config_load(app_config& cfg, const std::string& path)
{
    if (!std::filesystem::exists(path))
        return false;
    const auto result = rfl::toml::load<app_config>(path);
    if (result)
    {
        cfg = result.value();
        return true;
    }
    return false;
}

void app_config_save(const app_config& cfg, const std::string& path)
{
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    rfl::toml::save(path, cfg);
}
