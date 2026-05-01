#pragma once

#include <array>
#include <string>
#include <vector>

struct SDL_Window;

class OpenImageDialogs {
public:
    OpenImageDialogs();
    ~OpenImageDialogs() = default;

    OpenImageDialogs(const OpenImageDialogs &) = delete;
    OpenImageDialogs &operator=(const OpenImageDialogs &) = delete;

    void setup(SDL_Window *window);
    void begin_open_image_dialog();
    void open_online_popup();
    void draw_url_popup();

    void queue_path(const std::string &path);
    void queue_url(const std::string &url);

    [[nodiscard]] std::vector<std::string> take_pending_paths();
    [[nodiscard]] std::vector<std::string> take_pending_urls();

private:
    static void file_dialog_callback(void *userdata,
                                     const char *const *filelist,
                                     int filter);

    SDL_Window *m_window;
    std::vector<std::string> m_pending_paths;
    std::vector<std::string> m_pending_urls;
    bool m_show_url_popup;
    std::array<char, 512> m_url_buf;
};
