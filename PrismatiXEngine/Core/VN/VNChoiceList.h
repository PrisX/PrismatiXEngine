#pragma once

#include <string>
#include <vector>

namespace PrismatiX {
namespace UI {

struct VNChoice {
    std::string text;
    std::string target;
    std::string transitionStyle;
    std::string transitionSpeed;
    std::string transitionEase;
};

class VNChoiceList {
public:
    VNChoiceList() = default;
    ~VNChoiceList() = default;

    void AddChoice(const std::string& text, const std::string& target, const std::string& style = "", const std::string& speed = "", const std::string& ease = "");
    void Clear();
    bool HasChoices() const;
    const std::vector<VNChoice>& GetChoices() const { return choices; }

private:
    std::vector<VNChoice> choices;
};

}  // namespace UI
}  // namespace PrismatiX
