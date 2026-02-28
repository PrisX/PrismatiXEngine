#pragma once
#include <unordered_map>
#include <string>
#include <iostream>

class VariableManager {
private:
    static inline std::unordered_map<std::string, int> flags;

public:
    static void Set(const std::string& name, int value) {
        flags[name] = value;
    }

    static void Add(const std::string& name, int value) {
        flags[name] += value;
    }

    static int Get(const std::string& name) {
        return flags[name];
    }

    static bool Check(const std::string& name, const std::string& op, int compareVal) {
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
    static void Remove(const std::string& name) {
        flags.erase(name);
    }
    static void ClearAll() {
        flags.clear();
    }
};