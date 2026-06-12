#pragma once

#include "pch.hpp"

class sdl3_context
{
public:
    sdl3_context();

    sdl3_context(sdl3_context&& other) = default;

    SDL_Window* window;
    float       main_scale;

    bool init(const char* title, int width, int height);
    void shutdown();
};
