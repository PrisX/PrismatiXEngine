#include "Engine/UI/Startup/SplashTypes.h"

#include "Engine/Diagnostics/Diagnostic.h"

#include <cmath>

namespace px::ui::startup {
namespace {

diag::Diagnostic Error(std::string code, std::string message, const std::string& source,
                       const std::size_t index) {
    diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                                .code = std::move(code),
                                .category = "Player.Splash",
                                .message = std::move(message),
                                .details = "Splash entry index: " + std::to_string(index)};
    diagnostic.source.path = source;
    return diagnostic;
}

bool Number(const Variant& value, float& output) {
    if (const auto* number = value.TryGet<double>()) {
        output = static_cast<float>(*number);
        return std::isfinite(output);
    }
    if (const auto* integer = value.TryGet<std::int64_t>()) {
        output = static_cast<float>(*integer);
        return true;
    }
    return false;
}

}  // namespace

Status ValidateSplashEntry(const SplashScreenEntry& entry, const std::size_t index,
                           const std::string& source) {
    if (entry.scene.id.Empty() || entry.scene.lastKnownPath.empty())
        return Status::Fail(Error("PXBOOT1001", "Splash scene ResourceRef is required",
                                  source, index));
    if (entry.audio && (entry.audio->id.Empty() || entry.audio->lastKnownPath.empty()))
        return Status::Fail(Error("PXBOOT1002", "Splash audio ResourceRef is invalid",
                                  source, index));
    if (!std::isfinite(entry.minimumDuration) || entry.minimumDuration < 0.0f)
        return Status::Fail(Error("PXBOOT1003", "Splash minimumDuration must be non-negative",
                                  source, index));
    if (!std::isfinite(entry.skipAllowedAfter) || entry.skipAllowedAfter < 0.0f ||
        entry.skipAllowedAfter > entry.minimumDuration)
        return Status::Fail(Error(
            "PXBOOT1004",
            "Splash skipAllowedAfter must be within the minimum display duration", source,
            index));
    return Status::Ok();
}

Result<std::vector<SplashScreenEntry>> ParseSplashSequence(const Variant& value,
                                                            const std::string& source) {
    const auto* array = value.AsArray();
    if (!array)
        return Result<std::vector<SplashScreenEntry>>::Failure(
            Error("PXBOOT1010", "splashes must be an ordered Array", source, 0));

    std::vector<SplashScreenEntry> entries;
    entries.reserve(array->size());
    for (std::size_t index = 0; index < array->size(); ++index) {
        const auto* object = (*array)[index].AsObject();
        if (!object)
            return Result<std::vector<SplashScreenEntry>>::Failure(
                Error("PXBOOT1011", "Splash entry must be an Object", source, index));
        const auto scene = object->find("scene");
        const auto audio = object->find("audio");
        const auto minimum = object->find("minimumDuration");
        const auto skippable = object->find("skippable");
        const auto skipAfter = object->find("skipAllowedAfter");
        const auto enter = object->find("enterAnimation");
        const auto exit = object->find("exitAnimation");
        const auto* sceneRef =
            scene == object->end() ? nullptr : scene->second.TryGet<ResourceRefValue>();
        const auto* skippableValue =
            skippable == object->end() ? nullptr : skippable->second.TryGet<bool>();
        const auto* enterName =
            enter == object->end() ? nullptr : enter->second.TryGet<std::string>();
        const auto* exitName =
            exit == object->end() ? nullptr : exit->second.TryGet<std::string>();
        SplashScreenEntry entry;
        if (!sceneRef || !skippableValue || !enterName || !exitName ||
            minimum == object->end() || !Number(minimum->second, entry.minimumDuration) ||
            skipAfter == object->end() || !Number(skipAfter->second, entry.skipAllowedAfter))
            return Result<std::vector<SplashScreenEntry>>::Failure(
                Error("PXBOOT1012", "Splash entry fields are incomplete or invalid", source,
                      index));
        entry.scene = *sceneRef;
        entry.skippable = *skippableValue;
        entry.enterAnimation = *enterName;
        entry.exitAnimation = *exitName;
        if (audio != object->end() && audio->second.Type() != VariantType::Null) {
            const auto* audioRef = audio->second.TryGet<ResourceRefValue>();
            if (!audioRef)
                return Result<std::vector<SplashScreenEntry>>::Failure(
                    Error("PXBOOT1013", "Splash audio must be a ResourceRef or null", source,
                          index));
            entry.audio = *audioRef;
        }
        const Status valid = ValidateSplashEntry(entry, index, source);
        if (!valid)
            return Result<std::vector<SplashScreenEntry>>::Failure(valid.Diagnostics());
        entries.push_back(std::move(entry));
    }
    return Result<std::vector<SplashScreenEntry>>::Success(std::move(entries));
}

Variant WriteSplashSequence(const std::vector<SplashScreenEntry>& entries) {
    VariantArray values;
    values.reserve(entries.size());
    for (const auto& entry : entries) {
        values.emplace_back(VariantObject{
            {"scene", entry.scene},
            {"audio", entry.audio ? Variant(*entry.audio) : Variant{}},
            {"minimumDuration", static_cast<double>(entry.minimumDuration)},
            {"skippable", entry.skippable},
            {"skipAllowedAfter", static_cast<double>(entry.skipAllowedAfter)},
            {"enterAnimation", entry.enterAnimation},
            {"exitAnimation", entry.exitAnimation},
        });
    }
    return Variant(std::move(values));
}

}  // namespace px::ui::startup
