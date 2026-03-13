#include "BacklogManager.h"

std::vector<BacklogEntry> BacklogManager::logs;

void BacklogManager::AddLog(const std::string& speaker, const std::string& text, const std::string& voice) { logs.push_back({ speaker, text, voice, false }); }

void BacklogManager::AddChoice(const std::string& text) { logs.push_back({ "", text, "", true }); }

void BacklogManager::Clear() { logs.clear(); }

size_t BacklogManager::GetCount() { return logs.size(); }
