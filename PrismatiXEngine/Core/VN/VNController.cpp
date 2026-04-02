#include "VNController.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <sol/sol.hpp>

#include "Core/PrismatiXEngine.h"
#include "Managers/ArchiveManager.h"
#include "Managers/AssetManager.h"
#include "Managers/BacklogManager.h"
#include "Core/Systems/RenderSystem.h"
#include "Core/Systems/AudioSystem.h"
#include "Managers/UIManager.h"
#include "Managers/VariableManager.h"
#include "Utils/EasingUtils.h"
#include "Utils/TransitionUtils.h"

namespace {
constexpr int kScreenWidth = 1280;
constexpr int kScreenHeight = 720;

std::vector<std::string> SplitUtf8Chars(const std::string& text) {
    std::vector<std::string> chars;
    size_t i = 0;
    // UTF-8 神奇拆解，反正就是要用記憶體位置 https://stackoverflow.com/questions/45716356/utf-text-in-sdl2
    while (i < text.length()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if ((c & 0x80) == 0)
            len = 1;
        else if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;

        if (i + len > text.length()) {
            len = 1;
        }

        chars.push_back(text.substr(i, len));
        i += len;
    }

    return chars;
}

std::string ReadFirstArg(const std::map<std::string, std::string>& args, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        auto it = args.find(key);
        if (it != args.end() && !it->second.empty()) {
            return it->second;
        }
    }
    return "";
}

std::string ReadTransitionStyleArg(const std::map<std::string, std::string>& args) { return ReadFirstArg(args, { "transition", "trans", "style" }); }

std::string ReadTransitionSpeedArg(const std::map<std::string, std::string>& args) { return ReadFirstArg(args, { "transitionSpeed", "transitionspeed", "trans_speed", "tspeed", "speed" }); }

std::string ReadTransitionEaseArg(const std::map<std::string, std::string>& args) { return ReadFirstArg(args, { "ease", "easing" }); }

std::string ReadCharacterAnimationArg(const std::map<std::string, std::string>& args) { return ReadFirstArg(args, { "anim", "animation", "motion" }); }

std::string ReadCharacterAnimationEaseArg(const std::map<std::string, std::string>& args) { return ReadFirstArg(args, { "animease", "animationease", "ease", "easing" }); }

int ReadIntArg(const std::map<std::string, std::string>& args, std::initializer_list<const char*> keys, int fallback) {
    std::string value = ReadFirstArg(args, keys);
    if (value.empty()) return fallback;

    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

int ReadCharacterAnimationDurationArg(const std::map<std::string, std::string>& args) {
    int duration = ReadIntArg(args, { "animduration", "animframes", "duration", "frames", "speed" }, 18);
    return std::clamp(duration, 1, 120);
}

std::string ToLowerAlphaNumeric(const std::string& value);

std::string ReadSpeakerArg(const std::map<std::string, std::string>& args) { return ReadFirstArg(args, { "name" }); }

std::string ReadTextArg(const std::map<std::string, std::string>& args) { return ReadFirstArg(args, { "content" }); }

std::string ReadVarOperatorArg(const std::map<std::string, std::string>& args) { return ReadFirstArg(args, { "op" }); }

std::string ReadCharacterIdArg(const std::map<std::string, std::string>& args) { return ReadFirstArg(args, { "id" }); }

std::string ReadCharacterExpressionArg(const std::map<std::string, std::string>& args) { return ReadFirstArg(args, { "expression" }); }

int ReadCharacterSlotArg(const std::map<std::string, std::string>& args, int fallback = 1) {
    std::string value = ReadFirstArg(args, { "pos" });
    if (value.empty()) return fallback;

    std::string lowered = ToLowerAlphaNumeric(value);
    if (lowered == "left" || lowered == "l") return 0;
    if (lowered == "center" || lowered == "centre" || lowered == "middle" || lowered == "c") return 1;
    if (lowered == "right" || lowered == "r") return 2;

    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

bool ReadBoolArg(const std::map<std::string, std::string>& args, std::initializer_list<const char*> keys, bool fallback = false) {
    std::string value = ToLowerAlphaNumeric(ReadFirstArg(args, keys));
    if (value.empty()) return fallback;

    if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
    if (value == "0" || value == "false" || value == "no" || value == "off") return false;

    return fallback;
}

std::string ToLowerAlphaNumeric(const std::string& value) {
    std::string lowered;
    lowered.reserve(value.size());

    for (unsigned char ch : value) {
        if (std::isalnum(ch)) {
            lowered.push_back(static_cast<char>(std::tolower(ch)));
        }
    }

    return lowered;
}

std::string NormalizeCharacterAnimation(const std::string& value) {
    std::string lowered = ToLowerAlphaNumeric(value);

    if (lowered.empty() || lowered == "fade" || lowered == "default") return "fade";
    if (lowered == "none" || lowered == "instant") return "none";
    if (lowered == "slide" || lowered == "auto" || lowered == "slideauto") return "slide_auto";
    if (lowered == "slideleft" || lowered == "left") return "slide_left";
    if (lowered == "slideright" || lowered == "right") return "slide_right";
    if (lowered == "slideup" || lowered == "up") return "slide_up";
    if (lowered == "slidedown" || lowered == "down") return "slide_down";
    if (lowered == "pop" || lowered == "popin") return "pop";
    if (lowered == "bounce" || lowered == "jump") return "bounce";
    if (lowered == "zoom" || lowered == "zoomin") return "zoom";

    return "fade";
}

std::string ResolveCharacterAnimation(const ActiveCharacter& chara) {
    if (chara.animation == "slide_auto") {
        return (chara.targetX <= (kScreenWidth * 0.5f)) ? "slide_left" : "slide_right";
    }
    return chara.animation;
}

void StartCharacterAnimation(ActiveCharacter& chara, const std::map<std::string, std::string>& args) {
    chara.animation = NormalizeCharacterAnimation(ReadCharacterAnimationArg(args));
    chara.animationEase = ReadCharacterAnimationEaseArg(args);
    chara.animationDuration = ReadCharacterAnimationDurationArg(args);
    chara.animationFrame = 0;
    chara.animationActive = (chara.animation != "none");
    chara.renderOffsetX = 0.0f;
    chara.renderOffsetY = 0.0f;
    chara.renderScale = 1.0f;
}

void ApplyCharacterAnimationResult(ActiveCharacter& chara, const sol::table& result) {
    sol::optional<float> offsetX = result["offsetX"];
    if (offsetX) {
        chara.renderOffsetX = *offsetX;
    }

    sol::optional<float> offsetY = result["offsetY"];
    if (offsetY) {
        chara.renderOffsetY = *offsetY;
    }

    sol::optional<float> scale = result["scale"];
    if (scale) {
        chara.renderScale = *scale;
    }
}

void UpdateCharacterAnimation(ActiveCharacter& chara, sol::state& luaState) {
    chara.renderOffsetX = 0.0f;
    chara.renderOffsetY = 0.0f;
    chara.renderScale = 1.0f;

    if (!chara.animationActive) return;

    float duration = static_cast<float>(std::max(1, chara.animationDuration));
    float progress = std::clamp(static_cast<float>(chara.animationFrame) / duration, 0.0f, 1.0f);
    std::string animation = ResolveCharacterAnimation(chara);

    sol::object animationsObj = luaState["PortraitAnimations"];
    if (animationsObj.valid() && animationsObj.get_type() == sol::type::table) {
        sol::table animations = animationsObj.as<sol::table>();
        sol::protected_function fx = animations[animation];

        if (fx.valid()) {
            sol::table ctx = luaState.create_table();
            ctx["name"] = chara.name;
            ctx["diff"] = chara.diff;
            ctx["animation"] = chara.animation;
            ctx["resolvedAnimation"] = animation;
            ctx["trigger"] = chara.animationTrigger;
            ctx["ease"] = chara.animationEase;
            ctx["progress"] = progress;
            ctx["frame"] = chara.animationFrame;
            ctx["duration"] = chara.animationDuration;
            ctx["pos"] = chara.pos;
            ctx["currentX"] = chara.currentX;
            ctx["targetX"] = chara.targetX;
            ctx["screenWidth"] = kScreenWidth;
            ctx["screenHeight"] = kScreenHeight;

            sol::protected_function_result result = fx(ctx);
            if (!result.valid()) {
                sol::error err = result;
                std::cerr << "Portrait animation runtime error (" << animation << "): " << err.what() << std::endl;
                chara.animationActive = false;
            }
            else if (result.return_count() > 0) {
                sol::optional<sol::table> animResult = result;
                if (animResult) {
                    ApplyCharacterAnimationResult(chara, *animResult);
                }
            }
        }
    }

    if (progress >= 1.0f) {
        chara.animationActive = false;
        chara.renderOffsetX = 0.0f;
        chara.renderOffsetY = 0.0f;
        chara.renderScale = 1.0f;
        return;
    }

    chara.animationFrame++;
}

int FindLabelLine(const std::vector<VNCommand>& commands, const std::string& labelName) {
    for (size_t i = 0; i < commands.size(); ++i) {
        auto it = commands[i].args.find("name");
        if (commands[i].type == "label" && it != commands[i].args.end() && it->second == labelName) {
            return static_cast<int>(i);
        }
    }

    return -1;
}
}  // namespace

#include "VNControllerCommands.inl"
#include "VNControllerCore.inl"
#include "VNControllerRuntime.inl"
