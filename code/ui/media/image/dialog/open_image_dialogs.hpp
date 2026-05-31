#pragma once
#include "pch.hpp"

struct SDL_Window;

class OpenImageDialogs {
public:
    OpenImageDialogs();
    ~OpenImageDialogs() = default;

    OpenImageDialogs(const OpenImageDialogs &) = delete;
    OpenImageDialogs &operator=(const OpenImageDialogs &) = delete;

    void setup(SDL_Window *window);

    // Opens the native OS file picker dialog
    void begin_open_image_dialog();

    // Shows/draws the URL input popup
    void open_url_popup();
    void draw_url_popup();

    // Queue a local path or remote URL for processing
    void queue_path(const std::string &path);
    void queue_url(const std::string &url);

    // Consume pending items — caller takes ownership
    [[nodiscard]] std::vector<std::string> take_pending_paths();
    [[nodiscard]] std::vector<std::string> take_pending_urls();

    [[nodiscard]] bool has_pending_paths() const noexcept;
    [[nodiscard]] bool has_pending_urls()  const noexcept;

private:
    static void file_dialog_callback(void              *userdata,
                                     const char *const *filelist,
                                     int                filter);

    SDL_Window *m_window = nullptr;

    std::vector<std::string> m_pending_paths;
    std::vector<std::string> m_pending_urls;

    bool m_show_url_popup  = false;
    bool m_focus_url_input = false;

    static constexpr std::size_t k_url_buf_size = 2048;
    std::array<char, k_url_buf_size> m_url_buf{};
};