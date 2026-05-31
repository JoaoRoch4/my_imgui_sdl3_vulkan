#pragma once

#include "pch.hpp"

inline bool is_window_fullscreen(SDL_Window *window)
{
    if (!window)
        return false;
    return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
}

inline void set_window_fullscreen(SDL_Window *window, bool enabled)
{
    if (!window)
        return;
    SDL_SetWindowFullscreen(window, enabled);
}

inline void toggle_window_fullscreen(SDL_Window *window)
{
    set_window_fullscreen(window, !is_window_fullscreen(window));
}
