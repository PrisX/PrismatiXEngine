#pragma once

#include "Engine/Core/Variant.h"
#include "Engine/VN/Runtime/Backlog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/VM.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace px::script {

inline Variant BuildDialogueModel(const vn::Dialogue& dialogue) {
    const vn::DialogueState& state = dialogue.State();
    const auto color = [](const Color value) {
        return Variant(VariantObject{
            {"r", Variant(static_cast<std::int64_t>(value.r))},
            {"g", Variant(static_cast<std::int64_t>(value.g))},
            {"b", Variant(static_cast<std::int64_t>(value.b))},
            {"a", Variant(static_cast<std::int64_t>(value.a))}});
    };
    const double progress = state.totalChars > 0
                                ? std::clamp(
                                      static_cast<double>(state.currentChar) /
                                          state.totalChars,
                                      0.0, 1.0)
                                : 0.0;
    return Variant(VariantObject{
        {"active", Variant(!state.fullText.empty())},
        {"speaker", Variant(state.speaker)},
        {"fullText", Variant(state.fullText)},
        {"displayText", Variant(state.displayText)},
        {"voice", Variant(state.voice)},
        {"currentChar", Variant(state.currentChar)},
        {"totalChars", Variant(state.totalChars)},
        {"progress", Variant(progress)},
        {"finished", Variant(state.finished)},
        {"textColor", color(state.textColor)},
        {"outlineColor", color(state.outlineColor)},
        {"effect", Variant(state.effect)},
        {"effectProgress", Variant(state.effectProgress)}});
}

inline Variant BuildChoiceModels(const vn::VM& vm) {
    VariantArray values;
    const auto& choices = vm.Choices();
    values.reserve(choices.size());
    for (std::size_t index = 0; index < choices.size(); ++index) {
        const auto& choice = choices[index];
        values.emplace_back(VariantObject{
            {"index", Variant(static_cast<std::int64_t>(index))},
            {"text", Variant(choice.text)},
            {"target", Variant(choice.target)},
            {"sourceId", Variant(choice.sourceId)},
            {"operationId", Variant(choice.operationId)}});
    }
    return Variant(std::move(values));
}

inline Variant BuildBacklogModels(const vn::Backlog& backlog) {
    VariantArray values;
    const auto& entries = backlog.Entries();
    values.reserve(entries.size());
    for (const auto& entry : entries) {
        values.emplace_back(VariantObject{
            {"sequence", Variant(static_cast<std::int64_t>(
                 std::min<std::uint64_t>(
                     entry.sequence, static_cast<std::uint64_t>(
                                         (std::numeric_limits<std::int64_t>::max)()))))},
            {"speaker", Variant(entry.speaker)},
            {"text", Variant(entry.text)},
            {"voice", Variant(entry.voice)},
            {"isChoice", Variant(entry.isChoice)},
            {"sourceId", Variant(entry.sourceId)},
            {"operationId", Variant(entry.operationId)}});
    }
    return Variant(std::move(values));
}

}  // namespace px::script
