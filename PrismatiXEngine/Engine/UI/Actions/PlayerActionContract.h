#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace px::ui {

// The stock Player's UI actions: one table that the Player registers and
// dispatches from, that pxui validation checks authored actions against, and
// that the UI type manifest exports so the SDK can type-check TSX actions.
enum class PlayerActionArgument : std::uint8_t {
    None,
    Integer,  // also accepts an item placeholder such as "$index" or "$key"
    Boolean,
    // Text the handler parses itself, such as a page number or "key=value".
    Text,
};

// Which side runs the action: the Player host, or GalgameUI itself.
enum class PlayerActionOwner : std::uint8_t { Player, Ui };

struct PlayerActionSpec {
    std::string_view id;
    // The TSX `arguments` key whose value reaches the handler.
    std::string_view argumentKey;
    PlayerActionArgument argument = PlayerActionArgument::None;
    std::string_view category;
    PlayerActionOwner owner = PlayerActionOwner::Player;
};

namespace detail {
using enum PlayerActionArgument;
inline constexpr PlayerActionSpec Plain(std::string_view id, std::string_view category) {
    return {id, {}, None, category};
}
inline constexpr PlayerActionSpec With(std::string_view id, std::string_view key,
                                       PlayerActionArgument argument,
                                       std::string_view category) {
    return {id, key, argument, category};
}
}  // namespace detail

inline constexpr PlayerActionSpec kPlayerActions[] = {
    // Game
    detail::Plain("game.start", "Game"),
    detail::Plain("game.continue", "Game"),
    detail::Plain("app.quit", "Game"),
    detail::Plain("hud.hide", "Game"),
    // Stores the HUD TextInput field in the variable a Story [input] names.
    detail::Plain("input.accept", "Game"),
    detail::Plain("mode.auto", "Game"),
    detail::Plain("mode.skip", "Game"),
    detail::With("choice.select", "index", detail::Integer, "Game"),
    detail::Plain("choice.previous", "Game"),
    detail::Plain("choice.next", "Game"),
    detail::Plain("voice.replay", "Game"),
    detail::With("backlog.voice", "index", detail::Integer, "Backlog"),
    detail::With("backlog.rollback", "index", detail::Integer, "Backlog"),
    {"backlog.scroll", "argument", detail::Text, "Backlog", PlayerActionOwner::Ui},
    {"hud.toolbar.pin", {}, detail::None, "Game", PlayerActionOwner::Ui},
    // Screens
    detail::With("screen.open", "argument", detail::Text, "Navigation"),
    detail::Plain("title.open", "Navigation"),
    detail::Plain("save.open", "Navigation"),
    detail::Plain("load.open", "Navigation"),
    detail::Plain("settings.open", "Navigation"),
    detail::Plain("backlog.open", "Navigation"),
    detail::Plain("gallery.open", "Navigation"),
    detail::Plain("extra.open", "Navigation"),
    detail::Plain("replay.open", "Navigation"),
    detail::Plain("music.open", "Navigation"),
    detail::Plain("flowchart.open", "Navigation"),
    detail::Plain("overlay.close", "Navigation"),
    detail::Plain("settings.page0", "Navigation"),
    detail::Plain("settings.page1", "Navigation"),
    detail::Plain("settings.page2", "Navigation"),
    detail::Plain("settings.page3", "Navigation"),
    detail::Plain("settings.page4", "Navigation"),
    detail::Plain("settings.page5", "Navigation"),
    detail::Plain("settings.page6", "Navigation"),
    detail::Plain("settings.page7", "Navigation"),
    // Confirmations
    detail::Plain("dialog.accept", "Confirmation"),
    detail::Plain("dialog.cancel", "Confirmation"),
    detail::Plain("dialog.toggleSkip", "Confirmation"),
    detail::Plain("title.confirm.accept", "Confirmation"),
    detail::Plain("title.confirm.cancel", "Confirmation"),
    detail::Plain("system.confirm.accept", "Confirmation"),
    detail::Plain("system.confirm.cancel", "Confirmation"),
    // Save and load
    detail::With("save.slot", "slot", detail::Integer, "Save"),
    detail::With("load.slot", "slot", detail::Integer, "Save"),
    detail::With("saveLoad.page", "argument", detail::Text, "Save"),
    detail::Plain("saveLoad.confirm.accept", "Save"),
    detail::Plain("saveLoad.confirm.cancel", "Save"),
    detail::Plain("saveLoad.error.dismiss", "Save"),
    detail::Plain("save.quick", "Save"),
    detail::Plain("load.quick", "Save"),
    // Settings
    detail::With("set.master.value", "value", detail::Integer, "Settings"),
    detail::With("set.bgm.value", "value", detail::Integer, "Settings"),
    detail::With("set.se.value", "value", detail::Integer, "Settings"),
    detail::With("set.voice.value", "value", detail::Integer, "Settings"),
    detail::With("set.speed.value", "value", detail::Integer, "Settings"),
    detail::Plain("set.master.up", "Settings"),
    detail::Plain("set.master.down", "Settings"),
    detail::Plain("set.bgm.up", "Settings"),
    detail::Plain("set.bgm.down", "Settings"),
    detail::Plain("set.se.up", "Settings"),
    detail::Plain("set.se.down", "Settings"),
    detail::Plain("set.voice.up", "Settings"),
    detail::Plain("set.voice.down", "Settings"),
    detail::Plain("set.speed.up", "Settings"),
    detail::Plain("set.speed.down", "Settings"),
    detail::Plain("set.mute.master", "Settings"),
    detail::Plain("set.mute.bgm", "Settings"),
    detail::Plain("set.mute.se", "Settings"),
    detail::Plain("set.mute.voice", "Settings"),
    detail::Plain("set.duck.on", "Settings"),
    detail::Plain("set.duck.off", "Settings"),
    detail::Plain("set.voiceCarry.on", "Settings"),
    detail::Plain("set.voiceCarry.off", "Settings"),
    // "<profile id>:<0-128>"
    detail::With("set.voiceProfile.value", "argument", detail::Text, "Settings"),
    detail::With("set.voiceProfile.mute", "argument", detail::Text, "Settings"),
    detail::With("voiceProfile.test", "argument", detail::Text, "Settings"),
    detail::With("audio.test.se", "argument", detail::Text, "Settings"),
    detail::With("set.skipread.value", "value", detail::Boolean, "Settings"),
    detail::Plain("set.skipread.toggle", "Settings"),
    detail::With("set.fullscreen.value", "value", detail::Boolean, "Settings"),
    detail::Plain("set.fullscreen.on", "Settings"),
    detail::Plain("set.fullscreen.off", "Settings"),
    detail::Plain("set.fullscreen.toggle", "Settings"),
    detail::With("set.language.value", "locale", detail::Text, "Settings"),
    detail::With("set.textscale.value", "value", detail::Integer, "Accessibility"),
    detail::With("set.highcontrast.value", "value", detail::Boolean, "Accessibility"),
    detail::With("set.reducedmotion.value", "value", detail::Boolean, "Accessibility"),
    detail::Plain("set.reducedmotion.on", "Accessibility"),
    detail::Plain("set.reducedmotion.off", "Accessibility"),
    detail::With("set.selfvoicing.value", "value", detail::Boolean, "Accessibility"),
    detail::Plain("settings.reset", "Settings"),
    // A control that exists only for looks; activating it does nothing.
    detail::Plain("settings.noop", "Settings"),
    // "<option>=<integer>" for a player.options entry
    detail::With("option.set", "argument", detail::Text, "Settings"),
    // Music room
    detail::With("music.play", "argument", detail::Text, "Music"),
    detail::Plain("music.previous", "Music"),
    detail::Plain("music.next", "Music"),
    detail::Plain("music.random", "Music"),
    detail::Plain("music.stop", "Music"),
    detail::Plain("music.resume", "Music"),
    detail::Plain("music.about", "Music"),
    detail::Plain("music.scroll.home", "Music"),
    detail::Plain("music.scroll.up", "Music"),
    detail::Plain("music.scroll.down", "Music"),
    detail::Plain("music.scroll.end", "Music"),
    // Gallery, replay and flowchart
    detail::With("cg.view", "resource", detail::Text, "Gallery"),
    detail::With("replay.launch", "argument", detail::Text, "Replay"),
    detail::With("replay.page", "argument", detail::Text, "Replay"),
    // JSON object {"entry", "heading", "details"}
    detail::With("flowchart.select", "argument", detail::Text, "Flowchart"),
    detail::Plain("flowchart.jump", "Flowchart"),
    detail::With("flowchart.filter", "argument", detail::Text, "Flowchart"),
    detail::Plain("flowchart.scroll.home", "Flowchart"),
    detail::Plain("flowchart.scroll.up", "Flowchart"),
    detail::Plain("flowchart.scroll.down", "Flowchart"),
    detail::Plain("flowchart.scroll.end", "Flowchart"),
};

[[nodiscard]] inline const PlayerActionSpec* FindPlayerAction(const std::string_view id) {
    const auto found = std::ranges::find(kPlayerActions, id, &PlayerActionSpec::id);
    return found == std::end(kPlayerActions) ? nullptr : &*found;
}

[[nodiscard]] inline std::string_view PlayerActionArgumentName(
    const PlayerActionArgument argument) {
    switch (argument) {
    case PlayerActionArgument::Integer: return "integer";
    case PlayerActionArgument::Boolean: return "boolean";
    case PlayerActionArgument::Text: return "string";
    case PlayerActionArgument::None: break;
    }
    return "none";
}

}  // namespace px::ui
