#pragma once

#include "Applications/Player/QaHooks.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// QA IPC: a test interface of PrismatiXPlayerQA. The shipped PrismatiXPlayer
// compiles all of it out (QaIpc.cpp is empty there), so a game never opens a
// socket or carries these operation names.
//
// Enable it with PRISMATIX_QA_IPC=127.0.0.1:<port> (port 0 picks a free one;
// only 127.0.0.1, localhost and [::1] are accepted). The Player logs
// "QA IPC listening on 127.0.0.1:<port>" and writes the port number to the
// file PRISMATIX_QA_IPC_PORT_FILE names, if set. `prismatix test` sets both.
//
// One client at a time sends newline-delimited JSON objects and gets one
// response line per request, in order:
//   -> {"id": 7, "op": "click", "control": "Start"}
//   <- {"id": 7, "ok": true, "result": {...}}
//   <- {"id": 7, "ok": false, "error": {"code": "PXQA...", "message": "..."}}
// The socket thread only queues lines. The Player answers them on its main
// thread at the start of a frame, before input and UI update, so a request
// sees the state the previous frame presented. Waiting operations
// (waitFor, screenshot, jump) hold back later requests until they finish.
//
// Operations (fields after the op):
//   state                       app state, routes, UI screen/root, story
//                               position, dialogue, choices, VM, modes,
//                               settings, profile, idle
//   controls [prefix]           named controls of the mounted UI tree
//   click control               activate a named control (UI Activate event)
//   action action [argument]    a Player action through HandleUIAction, or
//                               an authored UI/script Action otherwise
//   key name [held]             a key press, by Input key name ("F8")
//   input action                a built-in input action ("accept", ...)
//   bindings action             keys bound to a named input action
//   uiData key [value]          read UI data, or write it when value is set
//   variable name               a Story/session variable
//   jump target                 start or restart the story at a label:
//                               "scene", "scene:label" or a raw IR label
//   advance                     what a click on the text does
//   advanceUntil condition timeoutMs maxLines
//                               click through lines (and timed waits), a few
//                               per frame, until the condition holds; fails
//                               at a choice or after maxLines (default 5000)
//   choose index                pick a presented choice
//   waitFor condition timeoutMs wait until every field of condition holds
//                               (see QaConditionHolds in QaIpc.cpp)
//   screenshot [path]           capture the next composed frame; writes a PNG
//                               when path is set, returns size/colors/hash
//   save slot                   save the state at the request, with the next
//                               Stage frame (no UI) as thumbnail; in a batch,
//                               at once and without a thumbnail
//   load slot                   load a slot synchronously
//   setting name [value]        read or change a setting (see QaSetting)
//   route operation [name]      open|show|push|replace|pop|showModal|closeModal
//   resource path               whether the mounted packages contain a file
//   log message                 write a line to the Player log
//   batch requests              run several non-waiting requests in one frame
//   quit                        answer, then exit the Player
namespace px::player {

class QaIpcServer {
public:
    struct Request {
        std::uint64_t connection = 0;
        std::string line;
    };

    ~QaIpcServer();
    // Binds a loopback endpoint ("127.0.0.1:0") and starts the socket thread.
    static std::unique_ptr<QaIpcServer> Start(std::string_view endpoint,
                                              std::string& error);
    [[nodiscard]] std::uint16_t Port() const { return m_port; }
    [[nodiscard]] std::vector<Request> TakeRequests();
    // Sends one response line to a connection; ignored once it has closed.
    void Send(std::uint64_t connection, const std::string& line);

private:
    QaIpcServer() = default;
    void Serve();

    std::intptr_t m_listener = -1;
    std::intptr_t m_client = -1;
    std::uint64_t m_connection = 0;
    std::uint16_t m_port = 0;
    std::atomic<bool> m_stopping{false};
    std::mutex m_mutex;
    std::mutex m_sendMutex;
    std::deque<Request> m_requests;
    std::thread m_thread;
};

// Player-side state of the QA IPC: the server and the request that is
// currently waiting for a later frame.
struct QaSession {
    struct Pending {
        std::uint64_t connection = 0;
        nlohmann::json id;
        std::uint64_t deadline = 0;
        // Returns a result once done; nullopt keeps waiting.
        std::function<std::optional<nlohmann::json>()> poll;
        std::string timeoutMessage;
    };
    std::unique_ptr<QaIpcServer> server;
    std::deque<QaIpcServer::Request> queue;
    std::optional<Pending> pending;
    int stableFrames = 0;
    // Runs once, after the next Stage frame is drawn and before the UI: a
    // `save` request takes its thumbnail there, as the Save screen does.
    std::function<void()> stageRendered;
};

// Errors a request handler throws; the code goes into the response.
struct QaError {
    std::string code;
    std::string message;
};

}  // namespace px::player
