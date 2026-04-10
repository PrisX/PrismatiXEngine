#include "VNChoiceList.h"

namespace PrismatiX {
namespace UI {

void VNChoiceList::AddChoice(const std::string& text, const std::string& target, const std::string& style, const std::string& speed, const std::string& ease) {
    VNChoice choice;
    choice.text = text;
    choice.target = target;
    choice.transitionStyle = style;
    choice.transitionSpeed = speed;
    choice.transitionEase = ease;
    choices.push_back(choice);
}

void VNChoiceList::Clear() {
    choices.clear();
}

bool VNChoiceList::HasChoices() const {
    return !choices.empty();
}

}  // namespace UI
}  // namespace PrismatiX
