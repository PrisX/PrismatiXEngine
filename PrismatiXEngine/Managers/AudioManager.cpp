#include "AudioManager.h"
#include <iostream>

std::unordered_map<std::string, Mix_Music*> AudioManager::bgmCache;
std::unordered_map<std::string, Mix_Chunk*> AudioManager::sfxCache;

Mix_Music* AudioManager::LoadBGM(const std::string& fileName) {
	if (bgmCache.find(fileName) != bgmCache.end()) {
		return bgmCache[fileName];
	}
	Mix_Music* bgm = Mix_LoadMUS(fileName.c_str());
	if (!bgm) {
		std::cerr << "Failed to load BGM (" << fileName << "): " << Mix_GetError() << std::endl;
	}
	bgmCache[fileName] = bgm;
	return bgm;
}

void AudioManager::PlayBGM(const std::string& fileName, int loops) {
	Mix_Music* bgm = LoadBGM(fileName);
	if (bgm) {
		Mix_PlayMusic(bgm, loops);
	}
}

void AudioManager::StopBGM() {
	Mix_HaltMusic();
}

Mix_Chunk* AudioManager::LoadSFX(const std::string& fileName) {
	if (sfxCache.find(fileName) != sfxCache.end()) {
		return sfxCache[fileName];
	}
	Mix_Chunk* sfx = Mix_LoadWAV(fileName.c_str());
	if (!sfx) {
		std::cerr << "Failed to load SFX (" << fileName << "): " << Mix_GetError() << std::endl;
	}
	sfxCache[fileName] = sfx;
	return sfx;
}

void AudioManager::PlaySFX(const std::string& fileName, int loops) {
	Mix_Chunk* sfx = LoadSFX(fileName);
	if (sfx) {
		Mix_PlayChannel(-1, sfx, loops); // -1 隨便選 channel
	}
}

void AudioManager::PlayVoice(const std::string& fileName) {
	Mix_Chunk* voice = LoadSFX(fileName);
	if (voice) {
		// 語音固定在 channel 0
		Mix_HaltChannel(0);
		Mix_PlayChannel(0, voice, 0);
	}
}

void AudioManager::StopVoice() {
	Mix_HaltChannel(0);
}

void AudioManager::CleanCache() {
	for (auto& pair : bgmCache) {
		if (pair.second) Mix_FreeMusic(pair.second);
	}
	bgmCache.clear();

	for (auto& pair : sfxCache) {
		if (pair.second) Mix_FreeChunk(pair.second);
	}
	sfxCache.clear();
}