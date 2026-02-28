#pragma once
#include <string>
#include <unordered_map>

class VariableManager {
private:
    static std::unordered_map<std::string, int> flags;

public:
    static void Set(const std::string& name, int value);
    static void Add(const std::string& name, int value);
    static int Get(const std::string& name);
    static bool Check(const std::string& name, const std::string& op, int compareVal);
    static void Remove(const std::string& name);
    static void ClearAll();
};