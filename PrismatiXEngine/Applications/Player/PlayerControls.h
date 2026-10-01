#pragma once

#include "Engine/UI/Control.h"

#include <string_view>

namespace px::player {

// Depth-first search of a mounted UI tree for the control the Player (or a
// QA journey) addresses by name.
template <typename T>
T* FindUiControlNamed(ui::Control* root, const std::string_view name) {
    if (!root) return nullptr;
    if (root->Name() == name) return dynamic_cast<T*>(root);
    for (const auto& child : root->Children()) {
        if (auto* control = dynamic_cast<ui::Control*>(child.get()))
            if (auto* found = FindUiControlNamed<T>(control, name))
                return found;
    }
    return nullptr;
}

}  // namespace px::player
