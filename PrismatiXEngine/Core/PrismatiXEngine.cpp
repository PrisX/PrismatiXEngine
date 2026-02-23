#include <SDL2/SDL.h>
#include <iostream>
#include "PrismatiXEngine.h"

PrismatiXEngine::PrismatiXEngine() : isRunning(false), window(nullptr), renderer(nullptr), backgroundTex(nullptr), characterTex(nullptr) {} // this is very important (trust me)
PrismatiXEngine::~PrismatiXEngine() { Clean(); }

bool PrismatiXEngine::Initialize(const std::string& title, int width, int height) { // 同標頭檔裡面的解釋
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) < 0) { // 其實應該可以不用加 Events
		std::cerr << "Failed to initialize SDL2: " << SDL_GetError() << std::endl;
		return false;
	}

	int imgFlags = IMG_INIT_PNG | IMG_INIT_JPG;
	if (!(IMG_Init(imgFlags) & imgFlags)) {
		std::cerr << "Failed to initialize SDL2_image: " << IMG_GetError() << std::endl;
		return false;
	}

	window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE); // 他不吃 std::string 所以轉 c_str
	if (!window) {
		std::cerr << "Failed to create window: " << SDL_GetError() << std::endl;
	}

	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1"); // 改用 Linear Filtering

	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC); // 硬體加速 & VSync
	if (!renderer) {
		std::cerr << "Failed to create renderer: " << SDL_GetError() << std::endl;
	}

	backgroundTex = TextureManager::LoadTexture("bg.jpg", renderer);
	characterTex = TextureManager::LoadTexture("girl.png", renderer);
	isRunning = true;
	return true;
}

void PrismatiXEngine::Run() {
	while (isRunning) {
		HandleEvents();
		Update();
		Render();
	}
}

void PrismatiXEngine::HandleEvents() {
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_QUIT) {
			isRunning = false;
		}
	}
}

void PrismatiXEngine::Update() {
	//
}

void PrismatiXEngine::Render() {
	if (backgroundTex) {
		TextureManager::Draw(backgroundTex, renderer, 0, 0, 1280, 720);
	}

	if (characterTex) {
		TextureManager::Draw(characterTex, renderer, 440, 120, 0.1f);
	}
	SDL_RenderPresent(renderer);
}

void PrismatiXEngine::Clean() {
	TextureManager::CleanCache();
	if (renderer) SDL_DestroyRenderer(renderer);
	if (window) SDL_DestroyWindow(window);
	IMG_Quit();
	SDL_Quit();
	std::cout << "Application destroyed." << std::endl;
}