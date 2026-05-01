#pragma once

#include <SDL3/SDL.h>

class sdl3_context
{
public:
    sdl3_context();

    SDL_Window* window;
    float       main_scale;

    bool init(const char* title, int width, int height);
    void shutdown();
};
