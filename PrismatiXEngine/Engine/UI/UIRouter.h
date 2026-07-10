#pragma once

#include "Engine/Core/Result.h"
#include "Engine/UI/Control.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace px::ui {

class UIRouter {
public:
    using ScreenFactory = std::function<Result<std::unique_ptr<Control>>() >;

    Status Register(std::string route, ScreenFactory factory);
    Status Navigate(std::string_view route, bool replace = false);
    Status Back();
    Status ShowModal(std::string_view route);
    Status CloseModal();
    [[nodiscard]] Control* Current() const;
    [[nodiscard]] Control* Modal() const { return m_modal.get(); }
    [[nodiscard]] std::string_view CurrentRoute() const;

private:
    struct Entry { std::string route; std::unique_ptr<Control> screen; };
    std::unordered_map<std::string, ScreenFactory> m_factories;
    std::vector<Entry> m_stack;
    std::unique_ptr<Control> m_modal;
};

}  // namespace px::ui
