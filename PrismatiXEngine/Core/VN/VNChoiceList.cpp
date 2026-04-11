#include "VNChoiceList.h"

namespace PrismatiX::VN {

void VNChoiceList::AddChoice(const std::string& text, const std::string& target, const std::string& style) {
    VNChoice choice;
    choice.text = text;
    choice.target = target;
    choice.transitionStyle = style;
    choices.push_back(choice);
}

void VNChoiceList::Clear() {
    choices.clear();
}

bool VNChoiceList::HasChoices() const {
    return !choices.empty();
}

} // namespace PrismatiX::UI
