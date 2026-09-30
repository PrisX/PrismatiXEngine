#pragma once

#include "Engine/SDK/Ui.h"

#include <optional>
#include <string>

namespace px::sdk {

// Writes a UI scene in the canonical .pxui wire shape that ParseUi reads.
// Covers the node tree and theme; returns nullopt for a document with a
// behavior graph, triggers, visual states or animations, which this writer
// does not encode yet. Used to export the built-in screens for authors to
// customise (`prismatix eject ui`).
[[nodiscard]] std::optional<std::string> WriteUiSceneJson(const UiDocument& document);

}  // namespace px::sdk
