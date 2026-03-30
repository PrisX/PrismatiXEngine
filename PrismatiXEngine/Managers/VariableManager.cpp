#include "VariableManager.h"

#include <iostream>

void VariableManager::Set(const std::string& name, int value) { flags[name] = value; }

void VariableManager::Add(const std::string& name, int value) { flags[name] += value; }

int VariableManager::Get(const std::string& name) {
    if (flags.find(name) != flags.end()) return flags[name];
    return 0;
}

const std::unordered_map<std::string, int>& VariableManager::GetAllFlags() const { return flags; }

bool VariableManager::Check(const std::string& name, const std::string& op, int compareVal) {
    int currentVal = Get(name);
    if (op == "==") return currentVal == compareVal;
    if (op == "!=") return currentVal != compareVal;
    if (op == ">") return currentVal > compareVal;
    if (op == "<") return currentVal < compareVal;
    if (op == ">=") return currentVal >= compareVal;
    if (op == "<=") return currentVal <= compareVal;
    return false;
}

void VariableManager::Remove(const std::string& name) { flags.erase(name); }

void VariableManager::ClearAll() { flags.clear(); }
