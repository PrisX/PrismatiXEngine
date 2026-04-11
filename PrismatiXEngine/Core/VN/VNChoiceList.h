#pragma once

#include <string>
#include <vector>

namespace PrismatiX::VN {

struct VNChoice {
    std::string text;
    std::string target;
    std::string transitionStyle;
};

class VNChoiceList {
public:
    VNChoiceList() = default;
    ~VNChoiceList() = default;

    void AddChoice(const std::string& text, const std::string& target, const std::string& style = "");
    void Clear();
    bool HasChoices() const;
    const std::vector<VNChoice>& GetChoices() const { return choices; }

private:
    std::vector<VNChoice> choices;
};

} // namespace PrismatiX::UI
