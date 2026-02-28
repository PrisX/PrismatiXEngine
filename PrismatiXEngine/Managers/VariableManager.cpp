#include "VariableManager.h"
#include <iostream>

std::unordered_map<std::string, int> VariableManager::flags;

void VariableManager::Set(const std::string& name, int value) {
    flags[name] = value;
}

void VariableManager::Add(const std::string& name, int value) {
    flags[name] += value;
}

int VariableManager::Get(const std::string& name) {
    return flags[name];
}

bool VariableManager::Check(const std::string& name, const std::string& op, int compareVal) {
    int val = Get(name);
    if (op == "==") return val == compareVal;
    if (op == "!=") return val != compareVal;
    if (op == ">")  return val > compareVal;
    if (op == "<")  return val < compareVal;
    if (op == ">=") return val >= compareVal;
    if (op == "<=") return val <= compareVal;

    std::cerr << "Unknown operator (" << op << "): " << std::endl;
    return false;
}

void VariableManager::Remove(const std::string& name) {
    flags.erase(name);
}

void VariableManager::ClearAll() {
    flags.clear();
}
