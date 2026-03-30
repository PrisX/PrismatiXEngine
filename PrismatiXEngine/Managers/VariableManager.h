#pragma once
#include <string>
#include <unordered_map>

class VariableManager {
private:
    std::unordered_map<std::string, int> flags;

public:
    VariableManager() = default;
    ~VariableManager() = default;

    void Set(const std::string& name, int value);
    void Add(const std::string& name, int value);
    int Get(const std::string& name);
    const std::unordered_map<std::string, int>& GetAllFlags() const;
    bool Check(const std::string& name, const std::string& op, int compareVal);
    void Remove(const std::string& name);
    void ClearAll();
};