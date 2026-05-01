#include "sdl3_context.hpp"
#include <stdio.h>

sdl3_context::sdl3_context()
    : window(nullptr)
    , main_scale(1.0f)
{
}

bool sdl3_context::init(const char* title, int width, int height)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS))
    {
        printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return false;
    }

    main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_WindowFlags window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_MAXIMIZED;
    window = SDL_CreateWindow(title, (int)(width * main_scale), (int)(height * main_scale), window_flags);
    if (window == nullptr)
    {
        printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return false;
    }

    return true;
}

void sdl3_context::shutdown()
{
    SDL_DestroyWindow(window);
    SDL_Quit();
}
