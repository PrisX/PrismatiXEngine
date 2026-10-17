#include "Applications/Player/QaIpc.h"

#if PRISMATIX_QA_HOOKS

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#endif

#include "Applications/Player/PlayerApp.h"
#include "Applications/Player/PlayerControls.h"
#include "Applications/Player/PlayerSupport.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Graphics/Screenshot.h"
#include "Engine/Support/Logger.h"
#include "Engine/Support/Utf8Path.h"
#include "Engine/UI/Widgets.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>

namespace px::player {

#if PRISMATIX_QA_HOOKS

namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
void CloseSocket(SocketHandle socket) { closesocket(socket); }
int PollOne(SocketHandle socket, int timeoutMs) {
    WSAPOLLFD entry{socket, POLLRDNORM, 0};
    return WSAPoll(&entry, 1, timeoutMs);
}
#else
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
void CloseSocket(SocketHandle socket) { ::close(socket); }
int PollOne(SocketHandle socket, int timeoutMs) {
    pollfd entry{socket, POLLIN, 0};
    return ::poll(&entry, 1, timeoutMs);
}
#endif

SocketHandle ToSocket(std::intptr_t value) { return static_cast<SocketHandle>(value); }
std::intptr_t FromSocket(SocketHandle socket) {
    return socket == kNoSocket ? -1 : static_cast<std::intptr_t>(socket);
}

constexpr std::size_t kMaxRequestBytes = 16u * 1024u * 1024u;

}  // namespace

std::unique_ptr<QaIpcServer> QaIpcServer::Start(const std::string_view endpoint,
                                                std::string& error) {
    const auto colon = endpoint.rfind(':');
    if (colon == std::string_view::npos) {
        error = "expected <host>:<port>";
        return nullptr;
    }
    std::string host(endpoint.substr(0, colon));
    const std::string portText(endpoint.substr(colon + 1));
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    const bool ipv6 = host == "::1";
    if (host == "localhost") host = "127.0.0.1";
    if (host != "127.0.0.1" && !ipv6) {
        error = "only loopback hosts (127.0.0.1, localhost, [::1]) are allowed";
        return nullptr;
    }
    int port = -1;
    try { port = std::stoi(portText); } catch (...) {}
    if (port < 0 || port > 65535 || portText.empty()) {
        error = "invalid port " + portText;
        return nullptr;
    }
#if defined(_WIN32)
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        error = "WSAStartup failed";
        return nullptr;
    }
#endif
    const SocketHandle listener = ::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kNoSocket) {
        error = "socket() failed";
        return nullptr;
    }
    sockaddr_storage storage{};
    socklen_t length = 0;
    if (ipv6) {
        auto& address = reinterpret_cast<sockaddr_in6&>(storage);
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_loopback;
        address.sin6_port = htons(static_cast<std::uint16_t>(port));
        length = sizeof(sockaddr_in6);
    } else {
        auto& address = reinterpret_cast<sockaddr_in&>(storage);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        length = sizeof(sockaddr_in);
    }
    if (::bind(listener, reinterpret_cast<sockaddr*>(&storage), length) != 0 ||
        ::listen(listener, 1) != 0) {
        CloseSocket(listener);
        error = "cannot listen on " + std::string(endpoint);
        return nullptr;
    }
    sockaddr_storage bound{};
    socklen_t boundLength = sizeof(bound);
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &boundLength);
    std::unique_ptr<QaIpcServer> server(new QaIpcServer());
    server->m_listener = FromSocket(listener);
    server->m_port = ntohs(ipv6 ? reinterpret_cast<sockaddr_in6&>(bound).sin6_port
                                : reinterpret_cast<sockaddr_in&>(bound).sin_port);
    server->m_thread = std::thread([raw = server.get()] { raw->Serve(); });
    return server;
}

QaIpcServer::~QaIpcServer() {
    m_stopping = true;
    if (m_thread.joinable()) m_thread.join();
    if (m_listener != -1) CloseSocket(ToSocket(m_listener));
#if defined(_WIN32)
    if (m_listener != -1) WSACleanup();
#endif
}

void QaIpcServer::Serve() {
    const SocketHandle listener = ToSocket(m_listener);
    while (!m_stopping) {
        if (PollOne(listener, 100) <= 0) continue;
        const SocketHandle client = ::accept(listener, nullptr, nullptr);
        if (client == kNoSocket) continue;
        int noDelay = 1;
        ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        std::uint64_t connection = 0;
        {
            const std::lock_guard lock(m_sendMutex);
            m_client = FromSocket(client);
            connection = ++m_connection;
        }
        std::string buffer;
        std::array<char, 64 * 1024> chunk{};
        while (!m_stopping) {
            const int ready = PollOne(client, 100);
            if (ready == 0) continue;
            if (ready < 0) break;
            const auto received = ::recv(client, chunk.data(), static_cast<int>(chunk.size()), 0);
            if (received <= 0) break;
            buffer.append(chunk.data(), static_cast<std::size_t>(received));
            for (auto newline = buffer.find('\n'); newline != std::string::npos;
                 newline = buffer.find('\n')) {
                std::string line = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                const std::lock_guard lock(m_mutex);
                m_requests.push_back({connection, std::move(line)});
            }
            if (buffer.size() > kMaxRequestBytes) break;
        }
        const std::lock_guard lock(m_sendMutex);
        CloseSocket(client);
        m_client = -1;
    }
}

std::vector<QaIpcServer::Request> QaIpcServer::TakeRequests() {
    const std::lock_guard lock(m_mutex);
    std::vector<Request> requests(std::make_move_iterator(m_requests.begin()),
                                  std::make_move_iterator(m_requests.end()));
    m_requests.clear();
    return requests;
}

void QaIpcServer::Send(const std::uint64_t connection, const std::string& line) {
    const std::lock_guard lock(m_sendMutex);
    if (connection != m_connection || m_client == -1) return;
    const SocketHandle client = ToSocket(m_client);
    std::string payload = line + "\n";
    std::size_t sent = 0;
    while (sent < payload.size()) {
        const auto written = ::send(client, payload.data() + sent,
                                    static_cast<int>(payload.size() - sent), 0);
        if (written <= 0) return;
        sent += static_cast<std::size_t>(written);
    }
}

// --- Player operations ------------------------------------------------------

namespace {

using Json = nlohmann::json;

[[noreturn]] void Fail(std::string code, std::string message) {
    throw QaError{std::move(code), std::move(message)};
}

Json VariantJson(const Variant& value) {
    switch (value.Type()) {
        case VariantType::Null: return nullptr;
        case VariantType::Bool: return *value.TryGet<bool>();
        case VariantType::Integer: return *value.TryGet<std::int64_t>();
        case VariantType::Number: return *value.TryGet<double>();
        case VariantType::String: return *value.TryGet<std::string>();
        case VariantType::Uuid: return value.TryGet<Uuid>()->ToString();
        case VariantType::ResourceRef: return value.TryGet<ResourceRefValue>()->lastKnownPath;
        case VariantType::Array: {
            Json result = Json::array();
            for (const auto& item : *value.AsArray()) result.push_back(VariantJson(item));
            return result;
        }
        case VariantType::Object: {
            Json result = Json::object();
            for (const auto& [name, item] : *value.AsObject()) result[name] = VariantJson(item);
            return result;
        }
        default: return Json{{"$type", static_cast<int>(value.Type())}};
    }
}

std::string_view VmStateName(const vn::VMState state) {
    switch (state) {
        case vn::VMState::Idle: return "idle";
        case vn::VMState::Running: return "running";
        case vn::VMState::WaitingClick: return "waitingClick";
        case vn::VMState::WaitingChoice: return "waitingChoice";
        case vn::VMState::WaitingTimer: return "waitingTimer";
        case vn::VMState::WaitingVideo: return "waitingVideo";
        case vn::VMState::WaitingExternal: return "waitingExternal";
        case vn::VMState::Paused: return "paused";
        case vn::VMState::Finished: return "finished";
    }
    return "unknown";
}

std::string_view ScreenName(const ui::GalgameUI::Screen screen) {
    using Screen = ui::GalgameUI::Screen;
    switch (screen) {
        case Screen::Title: return "title";
        case Screen::HUD: return "hud";
        case Screen::Backlog: return "backlog";
        case Screen::Save: return "save";
        case Screen::Load: return "load";
        case Screen::Gallery: return "gallery";
        case Screen::Settings: return "settings";
        case Screen::Video: return "video";
        case Screen::Custom: return "custom";
    }
    return "unknown";
}

// Built-in input actions, in InputAction order, spelled like the scripting API.
constexpr std::array<std::string_view, static_cast<std::size_t>(InputAction::Count)> kInputActions{
    "navigate-up", "navigate-down", "navigate-left", "navigate-right", "accept",
    "cancel", "focus-next", "focus-previous", "advance", "menu", "backlog",
    "toggle-auto", "toggle-skip", "toggle-ui", "quick-save", "quick-load",
    "open-save", "rollback", "pause"};

// FNV-1a 64 of the Story compiler (packages/authoring-sdk/src/fnv1a64.ts).
std::string Fnv1a64(const std::string_view text) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 0x100000001b3ull;
    }
    return std::format("{:016x}", hash);
}

// A Story project compiles scene `s` to the label pxscene.entry.<hash> and its
// authored label `*name` to pxscene.label.<hash>; older builds kept `s:top`.
std::vector<std::string> StoryLabelCandidates(const std::string& target) {
    std::vector<std::string> candidates{target};
    if (const auto colon = target.rfind(':'); colon != std::string::npos) {
        const std::string scene = target.substr(0, colon);
        const std::string label = target.substr(colon + 1);
        if (!label.empty() && label.front() == '*')
            candidates.push_back("pxscene.label." + Fnv1a64("scene-label:" + scene + ":" + label.substr(1)));
        candidates.push_back("pxscene.label." + Fnv1a64("scene-label:" + scene + ":" + label));
    } else {
        candidates.push_back("pxscene.entry." + Fnv1a64("scene-entry:" + target));
        candidates.push_back(target + ":top");
        candidates.push_back("pxscene.label." + Fnv1a64("scene-label:" + target + ":top"));
    }
    return candidates;
}

}  // namespace

// Everything the dispatcher needs from PlayerApp; a friend of it.
struct QaOps {
    using Json = nlohmann::json;

    static std::string AppState(const PlayerApp& app) {
        if (app.m_gameStartTransitionActive) return "starting";
        switch (app.m_appState) {
            case PlayerApp::AppState::BootSplash: return "bootSplash";
            case PlayerApp::AppState::Title: return "title";
            case PlayerApp::AppState::Game: return "game";
        }
        return "unknown";
    }

    static bool ScriptBusy(const PlayerApp& app) {
        return app.m_scriptHost &&
               (app.m_scriptHost->HasPendingAction() || app.m_scriptHost->HasPendingCommand());
    }

    // Nothing is loading, moving or typing: a screenshot now shows a settled
    // screen, and a click is answered by the screen the player sees.
    static Json Busy(PlayerApp& app) {
        auto& renderer = app.m_runtime.Renderer();
        const auto& transition = app.m_session->Routes().ActiveTransition();
        const auto effect = renderer.ActiveScreenEffect();
        Json busy = Json::object();
        const auto mark = [&](const char* name, bool value) { if (value) busy[name] = true; };
        mark("bootSplash", app.m_appState == PlayerApp::AppState::BootSplash);
        mark("titlePreload", !app.TitleVisualsReady());
        mark("routePreload", app.m_routePreloadBatch != 0);
        mark("gameStart", app.m_gameStartTransitionActive);
        mark("presentationHold", renderer.PresentationHoldActive());
        mark("routeTransition", transition && renderer.ScreenEffectPlaying(transition->handle));
        mark("screenEffect", effect && effect->elapsedSeconds < effect->durationSeconds);
        mark("script", ScriptBusy(app));
        mark("pendingSaveScreen", app.m_pendingSaveScreen);
        if (app.m_appState == PlayerApp::AppState::Game && !app.m_gameStartTransitionActive) {
            const auto& vm = app.m_session->VM();
            const auto state = vm.State();
            mark("story", state == vn::VMState::Running || state == vn::VMState::WaitingTimer ||
                              state == vn::VMState::WaitingVideo ||
                              state == vn::VMState::WaitingExternal);
            mark("typing", state == vn::VMState::WaitingClick && !app.m_session->Dialogue().Finished());
            const auto actors = app.m_session->Stage().Snapshot();
            mark("actors", std::ranges::any_of(actors, [](const auto& actor) {
                return actor.exiting || std::abs(actor.alpha - actor.targetAlpha) > 0.5f ||
                       std::abs(actor.x - actor.targetX) > 0.5f;
            }));
        }
        return busy;
    }

    static Json Story(PlayerApp& app, const bool withSource) {
        const auto& vm = app.m_session->VM();
        const auto& program = vm.CurrentProgram();
        const int pc = vm.ProgramCounter();
        std::string label;
        int labelIndex = -1;
        for (const auto& [name, index] : program.labels)
            if (index <= pc && index > labelIndex) { labelIndex = index; label = name; }
        Json story{{"script", app.m_script},
                   {"vmState", VmStateName(vm.State())},
                   {"programCounter", pc},
                   {"operationId", vm.CurrentOperationId()},
                   {"label", label},
                   {"chapter", vm.Chapter()},
                   {"bgm", vm.CurrentBgm()},
                   {"background", app.m_session->Stage().BackgroundPath()},
                   {"actors", app.m_session->Stage().Snapshot().size()}};
        if (!withSource) return story;
        Json source = nullptr;
        if (const auto* mapping = app.m_session->SourceMapping(vm.CurrentOperationId())) {
            source = Json{{"uri", mapping->sourceUri}, {"line", mapping->startLine}};
            const std::filesystem::path uri = px::PathFromUtf8(mapping->sourceUri);
            if (uri.extension() == ".pxstory") {
                const std::string scene = px::PathStemUtf8(mapping->sourceUri);
                story["scene"] = scene;
                if (label == "pxscene.entry." + Fnv1a64("scene-entry:" + scene))
                    story["sceneLabel"] = scene;
                else if (label == "pxscene.label." + Fnv1a64("scene-label:" + scene + ":top"))
                    story["sceneLabel"] = scene + ":top";
            }
        }
        story["source"] = source;
        return story;
    }

    static Json Settings(const PlayerApp& app) {
        const auto& settings = app.m_settings;
        Json options = Json::object();
        for (const auto& [name, spec] : app.m_optionSpecs) options[name] = app.OptionValue(name);
        return Json{{"masterVolume", settings.masterVolume}, {"bgmVolume", settings.bgmVolume},
                    {"seVolume", settings.seVolume}, {"voiceVolume", settings.voiceVolume},
                    {"masterMuted", settings.masterMuted}, {"bgmMuted", settings.bgmMuted},
                    {"seMuted", settings.seMuted}, {"voiceMuted", settings.voiceMuted},
                    {"voiceProfileVolumes", settings.voiceProfileVolumes},
                    {"voiceProfileMuted", settings.voiceProfileMuted},
                    {"textSpeedMs", settings.textSpeedMs}, {"autoWaitMs", settings.autoWaitMs},
                    {"skipReadOnly", settings.skipReadOnly}, {"fullscreen", settings.fullscreen},
                    {"windowWidth", settings.windowWidth}, {"windowHeight", settings.windowHeight},
                    {"language", settings.language}, {"autoMode", app.m_autoMode},
                    {"skipMode", app.m_skipMode}, {"options", options}};
    }

    static Json State(PlayerApp& app, const bool full) {
        auto& session = *app.m_session;
        const auto routes = session.Routes().CaptureState();
        const auto& dialogue = session.Dialogue().State();
        Json choices = Json::array();
        if (session.VM().State() == vn::VMState::WaitingChoice) {
            int index = 0;
            for (const auto& choice : session.VM().Choices())
                choices.push_back({{"index", index++}, {"text", choice.text}});
        }
        Json busy = Busy(app);
        Json state{
            {"appState", AppState(app)},
            {"route", session.Routes().CurrentRoute()},
            {"modal", session.Routes().CurrentModalRoute()},
            {"routes", {{"stack", routes.stack}, {"modals", routes.modals}}},
            {"ui", {{"route", app.m_ui.CurrentRoute()},
                    {"screen", ScreenName(app.m_ui.CurrentScreen())},
                    {"root", app.m_ui.Root() ? app.m_ui.Root()->Name() : std::string{}}}},
            {"idle", busy.empty()},
            {"busy", busy},
            {"story", Story(app, full)},
            {"dialogue", {{"active", session.Dialogue().Active()},
                          {"speaker", dialogue.speaker},
                          {"text", dialogue.fullText},
                          {"displayed", dialogue.displayText},
                          {"finished", dialogue.finished},
                          {"voice", dialogue.voice}}},
            {"choices", choices},
            {"modes", {{"auto", app.m_autoMode}, {"skip", app.m_skipMode},
                       {"hudHidden", app.m_hudHidden}}},
            {"dialog", app.m_dialog ? Json(app.m_dialog->kind) : Json(nullptr)},
            {"locale", app.m_settings.language},
        };
        // The startup screen on show while the Player boots (player.splashes).
        if (app.m_appState == PlayerApp::AppState::BootSplash && app.m_splash &&
            !app.m_splash->Completed()) {
            const std::size_t index = app.m_splash->CurrentIndex();
            state["splash"] = {{"index", index},
                               {"scene", index < app.m_boot.splashes.size()
                                   ? app.m_boot.splashes[index].scene.lastKnownPath : std::string{}}};
        }
        if (!full) return state;
        if (const auto runtimeState = app.m_ui.CaptureRuntimeState(); runtimeState.animation)
            state["ui"]["animationPosition"] = runtimeState.animation->position;
        const auto audio = app.m_runtime.Audio().CaptureState();
        std::string lastVoice;
        const auto& backlog = session.Backlog().Entries();
        if (const auto voiced = std::ranges::find_if(backlog.rbegin(), backlog.rend(),
                [](const vn::BacklogEntry& entry) { return !entry.voice.empty(); });
            voiced != backlog.rend())
            lastVoice = voiced->voice;
        state["audio"] = {{"music", audio.music.path}, {"voice", audio.voice.path},
                          {"mainVolume", app.m_runtime.Audio().MainVolume()}};
        state["backlog"] = {{"count", backlog.size()}, {"lastVoice", lastVoice}};
        state["locales"] = app.m_supportedLocales;
        state["settings"] = Settings(app);
        state["voiceProfiles"] = app.m_voiceProfileIds;
        state["profile"] = {{"clearCount", app.m_profile.ClearCount()},
                            {"cg", app.m_profile.UnlockedCGs()},
                            {"scenes", app.m_profile.UnlockedScenes().size()},
                            {"music", app.m_profile.UnlockedMusic().size()}};
        int width = 0, height = 0;
        SDL_Window* window = app.m_runtime.GetWindow().Handle();
        if (window) SDL_GetWindowSize(window, &width, &height);
        int logicalWidth = 0, logicalHeight = 0;
        app.m_runtime.Renderer().GetLogicalSize(logicalWidth, logicalHeight);
        state["window"] = {{"width", width}, {"height", height},
                           {"fullscreen", window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0},
                           {"logicalWidth", logicalWidth}, {"logicalHeight", logicalHeight}};
        state["saveDirectory"] = PathToUtf8(app.m_saveDirectory);
        return state;
    }

    static ui::Control* NamedControl(PlayerApp& app, const std::string& name) {
        return FindUiControlNamed<ui::Control>(app.m_ui.Root(), name);
    }

    static void ListControls(ui::Control& control, const std::string& prefix, Json& out) {
        if (!control.Name().empty() && control.Name().starts_with(prefix)) {
            Json entry{{"name", control.Name()}, {"visible", control.IsVisibleInTree()},
                       {"enabled", control.Enabled()},
                       {"button", dynamic_cast<ui::Button*>(&control) != nullptr}};
            if (const auto* label = dynamic_cast<ui::Label*>(&control)) entry["text"] = label->Text();
            else if (const auto* button = dynamic_cast<ui::Button*>(&control)) entry["text"] = button->Text();
            out.push_back(std::move(entry));
        }
        for (const auto& child : control.Children())
            if (auto* inner = dynamic_cast<ui::Control*>(child.get())) ListControls(*inner, prefix, out);
    }

    // Each field of the condition must hold. Strings compare exactly unless
    // the name says otherwise; unknown fields are an error, not a pass.
    static bool ConditionHolds(PlayerApp& app, const Json& condition, int& frames) {
        bool needsSource = condition.contains("scene") || condition.contains("sceneLabel");
        bool needsFull = condition.contains("uiAnimationStarted") || condition.contains("cgUnlocked");
        const Json state = State(app, needsSource || needsFull);
        bool holds = true;
        for (const auto& [key, expected] : condition.items()) {
            if (key == "appState" || key == "route" || key == "modal" || key == "locale" || key == "dialog") {
                holds = holds && state[key] == expected;
            } else if (key == "uiRoute" || key == "screen" || key == "uiRoot") {
                const char* field = key == "uiRoute" ? "route" : key == "screen" ? "screen" : "root";
                holds = holds && state["ui"][field] == expected;
            } else if (key == "vmState" || key == "label" || key == "scene" || key == "sceneLabel") {
                holds = holds && state["story"].value(key, Json()) == expected;
            } else if (key == "operation") {
                // The current Runtime IR operation: one id, or any of a list.
                const Json& current = state["story"]["operationId"];
                if (expected.is_string()) holds = holds && current == expected;
                else if (expected.is_array())
                    holds = holds && std::find(expected.begin(), expected.end(), current) != expected.end();
                else Fail("PXQA1001", "'operation' must be an operation id or a list of them");
            } else if (key == "speaker") {
                holds = holds && state["dialogue"]["speaker"] == expected;
            } else if (key == "textContains") {
                holds = holds && expected.is_string() &&
                        state["dialogue"]["text"].get<std::string>().find(expected.get<std::string>()) !=
                            std::string::npos;
            } else if (key == "dialogueActive") {
                holds = holds && state["dialogue"]["active"] == expected;
            } else if (key == "idle") {
                holds = holds && state["idle"] == expected;
            } else if (key == "scriptIdle") {
                holds = holds && (!state["busy"].contains("script")) == expected.get<bool>();
            } else if (key == "choices") {
                const auto count = state["choices"].size();
                holds = holds && (expected.is_boolean() ? (count > 0) == expected.get<bool>()
                                                        : expected.is_number() && count == expected.get<std::size_t>());
            } else if (key == "control") {
                const auto* control = expected.is_string() ? NamedControl(app, expected.get<std::string>()) : nullptr;
                holds = holds && control && control->IsVisibleInTree();
            } else if (key == "cgUnlocked") {
                holds = holds && expected.is_string() && app.m_profile.CGUnlocked(expected.get<std::string>());
            } else if (key == "uiAnimationStarted") {
                const bool started = state["ui"].contains("animationPosition") &&
                                     state["ui"]["animationPosition"].get<double>() > 0.0;
                holds = holds && started == expected.get<bool>();
            } else if (key == "frames") {
                holds = holds && ++frames > expected.get<int>();
            } else if (key == "stable") {
                // Checked by the caller.
            } else {
                Fail("PXQA1004", "unknown waitFor condition '" + key + "'");
            }
        }
        return holds;
    }

    static int Integer(const Json& request, const char* name) {
        if (!request.contains(name) || !request[name].is_number_integer())
            Fail("PXQA1001", std::string("'") + name + "' must be an integer");
        return request[name].get<int>();
    }

    static std::string Text(const Json& request, const char* name) {
        if (!request.contains(name) || !request[name].is_string() || request[name].get<std::string>().empty())
            Fail("PXQA1001", std::string("'") + name + "' must be a non-empty string");
        return request[name].get<std::string>();
    }

    static bool Flag(const Json& value) {
        if (value.is_boolean()) return value.get<bool>();
        if (value.is_number()) return value.get<double>() != 0.0;
        Fail("PXQA1001", "'value' must be a boolean");
    }

    static void RequireGame(const PlayerApp& app) {
        if (app.m_appState != PlayerApp::AppState::Game || app.m_gameStartTransitionActive)
            Fail("PXQA1005", "the story is not running (appState " + AppState(app) + ")");
    }

    static Json Setting(PlayerApp& app, const std::string& name, const Json* value) {
        static const std::map<std::string, std::string, std::less<>> buses{
            {"masterVolume", "master"}, {"bgmVolume", "bgm"}, {"seVolume", "se"},
            {"voiceVolume", "voice"}};
        auto& settings = app.m_settings;
        if (value) {
            if (const auto bus = buses.find(name); bus != buses.end()) {
                if (!value->is_number_integer()) Fail("PXQA1001", "'value' must be an integer");
                app.SetBusVolume(bus->second, value->get<int>());
            } else if (name == "textSpeedMs") {
                if (!value->is_number_integer()) Fail("PXQA1001", "'value' must be an integer");
                app.SetTextSpeed(value->get<int>());
            } else if (name == "autoWaitMs") {
                if (!value->is_number_integer()) Fail("PXQA1001", "'value' must be an integer");
                settings.autoWaitMs = std::max(0, value->get<int>());
            } else if (name == "skipReadOnly") {
                settings.skipReadOnly = Flag(*value);
            } else if (name == "fullscreen") {
                if (!app.ApplyFullscreenSetting(Flag(*value)))
                    Fail("PXQA1006", "the window mode could not be changed");
            } else if (name == "language") {
                if (!value->is_string() || !app.LoadLocale(value->get<std::string>(), true))
                    Fail("PXQA1006", "locale could not be loaded");
            } else if (name == "autoMode") {
                app.m_autoMode = Flag(*value);
                app.m_autoTimerStart = app.m_runtime.GetClock().NowMs();
            } else if (name == "skipMode") {
                app.m_skipMode = Flag(*value);
            } else if (name.starts_with("option.")) {
                if (!value->is_number_integer() || !app.SetOption(name.substr(7), value->get<int>()))
                    Fail("PXQA1006", "option " + name.substr(7) + " was not changed");
            } else {
                Fail("PXQA1007", "unknown setting '" + name + "'");
            }
        }
        const Json all = Settings(app);
        if (name.starts_with("option.")) {
            if (!app.m_optionSpecs.contains(name.substr(7))) Fail("PXQA1007", "unknown option '" + name.substr(7) + "'");
            return app.OptionValue(name.substr(7));
        }
        if (!all.contains(name)) Fail("PXQA1007", "unknown setting '" + name + "'");
        return all[name];
    }

    static void Jump(PlayerApp& app, const std::string& target) {
        const auto& program = app.m_session->VM().CurrentProgram();
        std::string label;
        // Runtime IR labels are stored as `@pxscene.label.<hash>`.
        for (const auto& candidate : StoryLabelCandidates(target)) {
            if (program.LabelIndex(candidate) >= 0) { label = candidate; break; }
            if (program.LabelIndex("@" + candidate) >= 0) { label = "@" + candidate; break; }
        }
        if (label.empty()) Fail("PXQA1008", "the story has no label for '" + target + "'");
        app.CloseAllModals();
        app.m_viewingCG.clear();
        app.m_session->Stage().ClearAll();
        app.m_session->Dialogue().Clear();
        app.m_session->Backlog().Clear();
        // A jump starts fresh text, like a new scene: leave any NVL page.
        app.m_nvlMode = false;
        app.m_nvlLines.clear();
        app.m_runtime.Audio().StopVoice();
        app.m_runtime.Audio().StopAmbience(0);
        app.m_runtime.Audio().StopBGM(0);
        app.m_autoMode = app.m_skipMode = app.m_hudHidden = false;
        app.m_rollback.clear();
        app.m_lastBacklogSequence = app.m_session->Backlog().LastSequence();
        if (!app.m_session->VM().RestartAtLabel(label))
            Fail("PXQA1008", "the story could not restart at " + label);
        if (app.m_session->Routes().CurrentRoute() != "hud")
            (void)app.m_session->Routes().Replace("hud", {.preset = "none"});
        PX_LOG_INFO("QA IPC jumped to story label target={} label={}", target, label);
    }

    // Activates a control the way the UI input router delivers a click or
    // keyboard accept: an Activate event bubbling from the control upwards.
    static void Click(PlayerApp& app, const std::string& name) {
        ui::Control* control = NamedControl(app, name);
        if (!control)
            Fail("PXQA1002", "no control named '" + name + "' on UI route '" +
                                 std::string(app.m_ui.CurrentRoute()) + "'");
        if (!control->IsVisibleInTree()) Fail("PXQA1003", "control '" + name + "' is not visible");
        if (!control->Enabled()) Fail("PXQA1003", "control '" + name + "' is disabled");
        ui::UIEvent event{.type = ui::UIEventType::Activate};
        for (ui::Control* node = control; node; node = dynamic_cast<ui::Control*>(node->Parent())) {
            node->HandleEvent(event);
            node->EmitInputSignal(event);
            if (event.stopPropagation ||
                (event.handled && node->GetMouseFilter() == ui::MouseFilter::Stop))
                break;
        }
    }

    static void Action(PlayerApp& app, const std::string& id, const std::string& argument) {
        if (PlayerApp::ActionHandlers().contains(id)) {
            app.HandleUIAction({id, argument});
            return;
        }
        ui::ActionInvocation invocation;
        invocation.action = id;
        if (!argument.empty()) invocation.arguments.emplace("argument", Variant(argument));
        invocation.context.sourceScene = std::string(app.m_ui.CurrentRoute());
        const auto started = app.m_ui.Actions().Start(std::move(invocation));
        if (!started) {
            const auto& diagnostics = started.Diagnostics();
            Fail("PXQA1009", "action '" + id + "' did not start" +
                                 (diagnostics.empty() ? std::string{} : ": " + diagnostics.front().message));
        }
    }

    static void Respond(PlayerApp& app, const std::uint64_t connection, const Json& id,
                        const Json& result) {
        app.m_qa->server->Send(connection, Json{{"id", id}, {"ok", true}, {"result", result}}.dump(
            -1, ' ', false, Json::error_handler_t::replace));
    }

    static void RespondError(PlayerApp& app, const std::uint64_t connection, const Json& id,
                             const QaError& error) {
        app.m_qa->server->Send(connection,
            Json{{"id", id}, {"ok", false},
                 {"error", {{"code", error.code}, {"message", error.message}}}}
                .dump(-1, ' ', false, Json::error_handler_t::replace));
    }

    // Runs one request. A waiting operation fills `pending` and returns
    // nullopt; it is only allowed outside a batch.
    static std::optional<Json> Handle(PlayerApp& app, const Json& request,
                                      std::optional<QaSession::Pending>* pending) {
        if (!request.is_object() || !request.contains("op") || !request["op"].is_string())
            Fail("PXQA1000", "a request is a JSON object with a string 'op'");
        const std::string op = request["op"].get<std::string>();
        const auto requireWaitable = [&] {
            if (!pending) Fail("PXQA1010", "'" + op + "' waits for later frames and cannot run in a batch");
        };
        const std::uint64_t now = SDL_GetTicks();
        if (op == "state") return State(app, true);
        if (op == "controls") {
            Json out = Json::array();
            if (app.m_ui.Root())
                ListControls(*app.m_ui.Root(), request.value("prefix", std::string{}), out);
            return out;
        }
        if (op == "click") {
            Click(app, Text(request, "control"));
            return Json::object();
        }
        if (op == "action") {
            Action(app, Text(request, "action"), request.value("argument", std::string{}));
            return Json::object();
        }
        if (op == "key") {
            const std::string name = Text(request, "name");
            const auto code = Input::KeyCode(name);
            if (!code) Fail("PXQA1011", "unknown key '" + name + "'");
            app.m_runtime.GetInput().InjectKeyPress(*code, request.value("held", false));
            return Json::object();
        }
        if (op == "tap") {
            if (!request.contains("x") || !request["x"].is_number() ||
                !request.contains("y") || !request["y"].is_number())
                Fail("PXQA1001", "'x' and 'y' must be numbers");
            // "secondary" is a right click (the game's right-click behaviour).
            const std::string button = request.value("button", std::string("primary"));
            if (button != "primary" && button != "secondary")
                Fail("PXQA1001", "'button' must be primary or secondary");
            if (button == "secondary")
                app.m_runtime.GetInput().InjectSecondaryClick(request["x"].get<float>(), request["y"].get<float>());
            else
                app.m_runtime.GetInput().InjectTap(request["x"].get<float>(), request["y"].get<float>());
            return Json::object();
        }
        if (op == "input") {
            const std::string name = Text(request, "action");
            const auto found = std::ranges::find(kInputActions, name);
            if (found == kInputActions.end()) Fail("PXQA1011", "unknown input action '" + name + "'");
            app.m_runtime.GetInput().InjectAction(
                static_cast<InputAction>(std::distance(kInputActions.begin(), found)));
            return Json::object();
        }
        if (op == "bindings") {
            const std::string action = Text(request, "action");
            Json keys = Json::array();
            for (const int key : app.m_runtime.GetInput().KeyBindings(action)) keys.push_back(Input::KeyName(key));
            return Json{{"exists", app.m_runtime.GetInput().HasAction(action)}, {"keys", keys}};
        }
        if (op == "uiData") {
            const std::string key = Text(request, "key");
            if (request.contains("value")) {
                const auto value = JsonVariant(request["value"]);
                if (!value) Fail("PXQA1001", "'value' cannot be converted to UI data");
                if (const Status written = app.m_ui.UpdateUiData(key, *value); !written)
                    Fail("PXQA1012", written.Diagnostics().empty() ? "UI data was not written"
                                                                   : written.Diagnostics().front().message);
            }
            const auto read = app.m_ui.ReadUiData(key);
            if (!read) Fail("PXQA1012", "no UI data at '" + key + "'");
            return VariantJson(read.Value());
        }
        if (op == "variable") {
            const auto* value = app.m_session->Variables().GetValue(Text(request, "name"));
            return value ? VariantJson(*value) : Json(nullptr);
        }
        if (op == "resource") return Json{{"exists", app.m_runtime.VFS().Exists(Text(request, "path"))}};
        if (op == "log") {
            PX_LOG_INFO("{}", Text(request, "message"));
            return Json::object();
        }
        if (op == "advance") {
            RequireGame(app);
            const auto vmState = app.m_session->VM().State();
            if (vmState == vn::VMState::WaitingChoice) Fail("PXQA1013", "a choice is waiting; use choose");
            if (vmState == vn::VMState::WaitingClick && !app.m_session->Dialogue().Finished()) {
                app.m_session->Dialogue().ShowAll();
                return Json{{"completedLine", true}};
            }
            if (!app.m_scriptServices.advanceDialogue()) Fail("PXQA1013", "the story did not advance");
            return Json{{"completedLine", false}};
        }
        if (op == "advanceUntil") {
            requireWaitable();
            if (app.m_appState != PlayerApp::AppState::Game && !app.m_gameStartTransitionActive)
                Fail("PXQA1005", "the story is not running (appState " + AppState(app) + ")");
            if (!request.contains("condition") || !request["condition"].is_object())
                Fail("PXQA1001", "'condition' must be an object");
            const Json condition = request["condition"];
            const int timeoutMs = request.value("timeoutMs", 120'000);
            const int maxLines = request.value("maxLines", 5'000);
            auto frames = std::make_shared<int>(0);
            auto lines = std::make_shared<int>(0);
            *pending = QaSession::Pending{
                .deadline = now + static_cast<std::uint64_t>(std::max(0, timeoutMs)),
                .poll = [&app, condition, frames, lines, maxLines]() -> std::optional<Json> {
                    const auto done = [&]() -> std::optional<Json> {
                        Json state = State(app, true);
                        state["advancedLines"] = *lines;
                        return state;
                    };
                    if (ConditionHolds(app, condition, *frames)) return done();
                    // A new game is still fading in.
                    if (app.m_gameStartTransitionActive) return std::nullopt;
                    if (app.m_appState != PlayerApp::AppState::Game)
                        Fail("PXQA1013", "the story left the game before the condition held");
                    // A menu or a transition: wait for the story to be clickable.
                    if (!app.m_session->Routes().CurrentModalRoute().empty()) return std::nullopt;
                    auto& vm = app.m_session->VM();
                    if (vm.State() == vn::VMState::Finished || vm.State() == vn::VMState::Idle)
                        Fail("PXQA1013", std::format("the story ended after {} line(s) and the "
                                                     "condition does not hold", *lines));
                    if (vm.State() == vn::VMState::WaitingChoice)
                        Fail("PXQA1013", std::format("a choice is waiting after {} line(s) and the "
                                                     "condition does not hold; choose first", *lines));
                    // A few lines per frame, checking the condition after
                    // each so a matching line is never clicked past.
                    for (int step = 0; step < 8; ++step) {
                        const auto state = vm.State();
                        if (state != vn::VMState::WaitingClick && state != vn::VMState::WaitingTimer) break;
                        if (++*lines > maxLines)
                            Fail("PXQA1013", std::format("the condition did not hold within {} lines",
                                                         maxLines));
                        if (state == vn::VMState::WaitingClick && !app.m_session->Dialogue().Finished())
                            app.m_session->Dialogue().ShowAll();
                        if (!app.m_scriptServices.advanceDialogue()) break;
                        int noFrames = 0;
                        if (ConditionHolds(app, condition, noFrames)) return done();
                    }
                    return std::nullopt;
                },
                .timeoutMessage = "advanceUntil condition not met: " + condition.dump()};
            return std::nullopt;
        }
        if (op == "choose") {
            RequireGame(app);
            const int index = Integer(request, "index");
            const auto& choices = app.m_session->VM().Choices();
            if (app.m_session->VM().State() != vn::VMState::WaitingChoice)
                Fail("PXQA1013", "no choice is waiting");
            if (index < 0 || static_cast<std::size_t>(index) >= choices.size())
                Fail("PXQA1001", std::format("choice {} is outside 0..{}", index, choices.size() - 1));
            const std::string text = choices[static_cast<std::size_t>(index)].text;
            // The presented choice button first, like a click; the script
            // service when the HUD has not laid the choices out.
            if (!app.m_ui.ActivateChoice(static_cast<std::size_t>(index)) &&
                !app.m_scriptServices.selectChoice(index))
                Fail("PXQA1013", "the choice was not accepted");
            return Json{{"text", text}};
        }
        if (op == "save") {
            RequireGame(app);
            const int slot = Integer(request, "slot");
            // A batch is one frame: save its state at once, without a
            // thumbnail (the frame is not drawn yet).
            if (!pending) {
                if (!app.SaveSlot(slot, {})) Fail("PXQA1014", std::format("slot {} was not saved", slot));
                return Json::object();
            }
            if (app.m_qa->stageRendered) Fail("PXQA1014", "another save is waiting for its frame");
            auto snapshot = app.MakeSnapshot(/*includeBacklog=*/true);
            if (!snapshot) Fail("PXQA1014", std::format("slot {} has no state to save", slot));
            auto written = std::make_shared<std::optional<bool>>();
            app.m_qa->stageRendered = [&app, slot, written,
                                       state = std::make_shared<progress::SaveSnapshot>(
                                           std::move(*snapshot))]() mutable {
                state->thumbnailPng = graphics::CaptureThumbnailPng(
                    app.m_runtime.Renderer().Handle(), 256, 144);
                *written = app.WriteSaveSlot(slot, std::move(*state));
            };
            *pending = QaSession::Pending{
                .deadline = now + 10'000,
                .poll = [written, slot]() -> std::optional<Json> {
                    if (!*written) return std::nullopt;
                    if (!**written) Fail("PXQA1014", std::format("slot {} was not saved", slot));
                    return Json{{"thumbnail", true}};
                },
                .timeoutMessage = "no Stage frame was drawn to save with"};
            return std::nullopt;
        }
        if (op == "load") {
            const int slot = Integer(request, "slot");
            if (!app.LoadSlot(slot)) Fail("PXQA1014", std::format("slot {} was not loaded", slot));
            return Json::object();
        }
        if (op == "setting") {
            const std::string name = Text(request, "name");
            return Setting(app, name, request.contains("value") ? &request["value"] : nullptr);
        }
        if (op == "route") {
            const std::string operation = Text(request, "operation");
            const std::string name = request.value("name", std::string{});
            auto& routes = app.m_session->Routes();
            Status status = Status::Ok();
            if (operation == "open") app.OpenScreen(Text(request, "name"));
            else if (operation == "push") status = routes.Push(Text(request, "name"));
            else if (operation == "replace") status = routes.Replace(Text(request, "name"));
            else if (operation == "show") status = routes.Navigate(Text(request, "name"));
            else if (operation == "showModal") status = routes.ShowModal(Text(request, "name"));
            else if (operation == "closeModal") status = routes.CloseModal();
            else if (operation == "pop") status = routes.Back();
            else Fail("PXQA1001", "unknown route operation '" + operation + "'");
            if (!status)
                Fail("PXQA1015", status.Diagnostics().empty() ? "route operation failed"
                                                              : status.Diagnostics().front().message);
            const auto captured = routes.CaptureState();
            return Json{{"stack", captured.stack}, {"modals", captured.modals}};
        }
        if (op == "batch") {
            if (!request.contains("requests") || !request["requests"].is_array())
                Fail("PXQA1001", "'requests' must be an array");
            Json results = Json::array();
            for (const auto& inner : request["requests"]) results.push_back(*Handle(app, inner, nullptr));
            return results;
        }
        if (op == "quit") {
            app.m_quitRequested = true;
            return Json::object();
        }
        if (op == "jump") {
            requireWaitable();
            const std::string target = Text(request, "target");
            if (app.m_appState == PlayerApp::AppState::Game && !app.m_gameStartTransitionActive) {
                Jump(app, target);
                return State(app, true);
            }
            if (app.m_appState != PlayerApp::AppState::Title && !app.m_gameStartTransitionActive)
                Fail("PXQA1005", "jump needs the title or a running story (appState " + AppState(app) + ")");
            // From the title: start a new game, then restart it at the label
            // the moment it has committed, before its first story frame.
            app.StartGame();
            *pending = QaSession::Pending{
                .deadline = now + 60'000,
                .poll = [&app, target]() -> std::optional<Json> {
                    if (app.m_appState != PlayerApp::AppState::Game) return std::nullopt;
                    Jump(app, target);
                    return State(app, true);
                },
                .timeoutMessage = "the game did not start"};
            return std::nullopt;
        }
        if (op == "waitFor") {
            requireWaitable();
            if (!request.contains("condition") || !request["condition"].is_object())
                Fail("PXQA1001", "'condition' must be an object");
            const Json condition = request["condition"];
            const int stable = condition.value("stable", 1);
            const int timeoutMs = request.value("timeoutMs", 30'000);
            auto frames = std::make_shared<int>(0);
            auto held = std::make_shared<int>(0);
            *pending = QaSession::Pending{
                .deadline = now + static_cast<std::uint64_t>(std::max(0, timeoutMs)),
                .poll = [&app, condition, stable, frames, held]() -> std::optional<Json> {
                    if (!ConditionHolds(app, condition, *frames)) { *held = 0; return std::nullopt; }
                    if (++*held < stable) return std::nullopt;
                    return State(app, true);
                },
                .timeoutMessage = "condition not met: " + condition.dump()};
            // A condition may already hold; answering this frame keeps
            // `waitFor` cheap after an operation that settled synchronously.
            return std::nullopt;
        }
        if (op == "screenshot") {
            requireWaitable();
            std::filesystem::path path;
            if (request.contains("path")) path = PathFromUtf8(Text(request, "path"));
            int width = 0, height = 0;
            app.m_runtime.Renderer().GetLogicalSize(width, height);
            if (request.value("native", false)) {
                SDL_GetCurrentRenderOutputSize(app.m_runtime.Renderer().Handle(), &width, &height);
            }
            auto result = std::make_shared<std::optional<Json>>();
            app.m_runtime.OnNextComposedFrame([&app, path, width, height, result] {
                SDL_Renderer* renderer = app.m_runtime.Renderer().Handle();
                const auto summary = graphics::CaptureFrameSummary(renderer);
                Json captured{{"width", width}, {"height", height},
                              {"sampledColors", summary.sampledColors},
                              {"hash", std::format("{:016x}", summary.hash)},
                              {"valid", summary.Valid()}};
                if (!path.empty()) {
                    const auto png = graphics::CaptureThumbnailPng(renderer, width, height);
                    std::error_code error;
                    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
                    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                    stream.write(reinterpret_cast<const char*>(png.data()),
                                 static_cast<std::streamsize>(png.size()));
                    captured["path"] = PathToUtf8(path);
                    captured["written"] = !png.empty() && stream.good();
                }
                *result = std::move(captured);
            });
            *pending = QaSession::Pending{
                .deadline = now + 10'000,
                .poll = [result]() -> std::optional<Json> {
                    if (!*result) return std::nullopt;
                    if ((*result)->contains("written") && !(*result)->at("written").get<bool>())
                        Fail("PXQA1016", "the screenshot could not be written");
                    return **result;
                },
                .timeoutMessage = "no frame was composed"};
            return std::nullopt;
        }
        Fail("PXQA1000", "unknown op '" + op + "'");
    }

    static void Pump(PlayerApp& app) {
        auto& qa = *app.m_qa;
        for (auto& request : qa.server->TakeRequests()) qa.queue.push_back(std::move(request));
        while (!app.m_quitRequested || qa.pending) {
            if (qa.pending) {
                auto& pending = *qa.pending;
                try {
                    if (auto result = pending.poll()) {
                        Respond(app, pending.connection, pending.id, *result);
                        qa.pending.reset();
                        continue;
                    }
                    if (SDL_GetTicks() >= pending.deadline) {
                        const Json now = State(app, false);
                        std::string busy;
                        for (const auto& [reason, flag] : now["busy"].items()) busy += (busy.empty() ? "" : ",") + reason;
                        throw QaError{"PXQA1100", std::format(
                            "timed out: {} (appState={} route={} modal={} ui={} vm={} busy=[{}])",
                            pending.timeoutMessage, now["appState"].get<std::string>(),
                            now["route"].get<std::string>(), now["modal"].get<std::string>(),
                            now["ui"]["route"].get<std::string>(),
                            now["story"]["vmState"].get<std::string>(), busy)};
                    }
                } catch (const QaError& error) {
                    RespondError(app, pending.connection, pending.id, error);
                    qa.pending.reset();
                    continue;
                } catch (const std::exception& error) {
                    RespondError(app, pending.connection, pending.id, {"PXQA1999", error.what()});
                    qa.pending.reset();
                    continue;
                }
                return;
            }
            if (qa.queue.empty() || app.m_quitRequested) return;
            const auto request = std::move(qa.queue.front());
            qa.queue.pop_front();
            Json id = nullptr;
            try {
                const Json parsed = Json::parse(request.line);
                if (parsed.is_object() && parsed.contains("id")) id = parsed["id"];
                std::optional<QaSession::Pending> pending;
                if (auto result = Handle(app, parsed, &pending)) {
                    Respond(app, request.connection, id, *result);
                } else if (pending) {
                    pending->connection = request.connection;
                    pending->id = id;
                    qa.pending = std::move(pending);
                }
            } catch (const QaError& error) {
                RespondError(app, request.connection, id, error);
            } catch (const std::exception& error) {
                RespondError(app, request.connection, id, {"PXQA1000", error.what()});
            }
        }
    }
};

void PlayerApp::StartQaIpc() {
    const char* endpoint = PX_QA_ENV("PRISMATIX_QA_IPC");
    if (!endpoint || !*endpoint) return;
    std::string error;
    auto server = QaIpcServer::Start(endpoint, error);
    if (!server) {
        PX_LOG_ERROR("QA IPC could not start endpoint={} error={}", endpoint, error);
        return;
    }
    const auto port = server->Port();
    m_qa = std::make_unique<QaSession>();
    m_qa->server = std::move(server);
    PX_LOG_INFO("QA IPC listening on 127.0.0.1:{}", port);
    if (const char* portFile = PX_QA_ENV("PRISMATIX_QA_IPC_PORT_FILE"); portFile && *portFile) {
        // Written beside the target and renamed, so a reader never sees a
        // partial number.
        const std::filesystem::path target = PathFromUtf8(portFile);
        std::filesystem::path temporary = target;
        temporary += ".tmp";
        std::error_code ignored;
        if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ignored);
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream << port << '\n';
        }
        std::filesystem::rename(temporary, target, ignored);
        if (ignored) PX_LOG_ERROR("QA IPC could not write port file={}", portFile);
    }
}

void PlayerApp::PumpQaIpc() {
    if (m_qa && m_qa->server && m_session) QaOps::Pump(*this);
}

void PlayerApp::QaStageRendered() {
    if (m_qa && m_qa->stageRendered) std::exchange(m_qa->stageRendered, {})();
}

#else

QaIpcServer::~QaIpcServer() = default;
void PlayerApp::StartQaIpc() {}
void PlayerApp::PumpQaIpc() {}
void PlayerApp::QaStageRendered() {}

#endif

}  // namespace px::player
