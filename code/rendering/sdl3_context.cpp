#include "pch.hpp"
#include <cstdio>
#include <format>

#include "sdl3_context.hpp"

sdl3_context::sdl3_context()
	: window(nullptr)
	, main_scale(1.0f) { }

bool sdl3_context::init(char const* title, int width, int height) {
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {


char err[256];
		sprintf(err, "Error: SDL_Init(): %s \n", (SDL_GetError()));
		std::cerr << err;
		throw std::runtime_error(err);
		return false;
	}

	main_scale                   = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
	SDL_WindowFlags window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN
		| SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_MAXIMIZED;
	window = SDL_CreateWindow(title, (int)(width * main_scale), (int)(height * main_scale), window_flags);
	if (window == nullptr) {
		auto err = std::format("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
		std::printf("%s", err.c_str());
		throw std::runtime_error(err);
		return false;
	}

	return true;
}

void sdl3_context::shutdown() {
	SDL_DestroyWindow(window);
	SDL_Quit();
}
