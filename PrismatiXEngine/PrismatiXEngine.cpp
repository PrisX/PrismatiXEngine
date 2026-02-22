#include <SDL2/SDL.h>
#include <iostream>
#include "PrismatiXEngine.h"

int main(int argc, char* argv[]) {
	SDL_Init(SDL_INIT_EVERYTHING);

	SDL_Window* window = SDL_CreateWindow("PrismatiX VN Engine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, SDL_WINDOW_SHOWN);

	bool isRunning = true;

	SDL_Event event;
	while (isRunning) {
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) {
				std::cout << "Closing.." << std::endl;
				isRunning = false;
			}
		}
	}

	return 0;
}