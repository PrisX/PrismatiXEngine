#include "Engine/Platform/ProcessWatch.h"

#include <chrono>
#include <stop_token>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#elif !defined(__EMSCRIPTEN__)
#include <signal.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace px::platform {

std::optional<std::uint64_t> ParentProcessId() {
#if defined(_WIN32)
    const DWORD self = GetCurrentProcessId();
    std::optional<std::uint64_t> parent;
    if (HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL found = Process32FirstW(snapshot, &entry); found;
             found = Process32NextW(snapshot, &entry)) {
            if (entry.th32ProcessID == self) {
                if (entry.th32ParentProcessID != 0) parent = entry.th32ParentProcessID;
                break;
            }
        }
        CloseHandle(snapshot);
    }
    return parent;
#elif defined(__EMSCRIPTEN__)
    return std::nullopt;
#else
    const pid_t parent = getppid();
    if (parent <= 1) return std::nullopt;
    return static_cast<std::uint64_t>(parent);
#endif
}

std::function<bool()> ProcessAliveProbe(const std::uint64_t pid) {
#if defined(_WIN32)
    if (pid == 0 || pid > 0xffffffffull) return {};
    HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                static_cast<DWORD>(pid));
    if (!handle) {
        // An id that no longer names a process: the owner is already gone.
        if (GetLastError() == ERROR_INVALID_PARAMETER) return [] { return false; };
        return {};
    }
    FILETIME created{}, selfCreated{}, unused1{}, unused2{}, unused3{};
    if (GetProcessTimes(handle, &created, &unused1, &unused2, &unused3) &&
        GetProcessTimes(GetCurrentProcess(), &selfCreated, &unused1, &unused2, &unused3) &&
        CompareFileTime(&created, &selfCreated) > 0) {
        CloseHandle(handle);
        return [] { return false; };
    }
    const std::shared_ptr<void> owned(handle, CloseHandle);
    return [owned] { return WaitForSingleObject(owned.get(), 0) == WAIT_TIMEOUT; };
#elif defined(__EMSCRIPTEN__)
    (void)pid;
    return {};
#else
    if (pid <= 1) return {};
    const auto watched = static_cast<pid_t>(pid);
    // The parent is the common case; getppid() changes when it exits, which
    // stays correct even if its id is reused later.
    if (getppid() == watched) return [watched] { return getppid() == watched; };
    return [watched] { return kill(watched, 0) == 0 || errno == EPERM; };
#endif
}

struct ProcessExitWatch::State {
    std::jthread thread;
};

ProcessExitWatch::ProcessExitWatch(std::unique_ptr<State> state) : m_state(std::move(state)) {}

ProcessExitWatch::~ProcessExitWatch() {
    if (m_state && m_state->thread.joinable()) {
        m_state->thread.request_stop();
        m_state->thread.join();
    }
}

std::unique_ptr<ProcessExitWatch> ProcessExitWatch::Start(std::function<bool()> alive,
                                                          std::function<void()> onExit) {
    if (!alive || !onExit) return nullptr;
    auto state = std::make_unique<State>();
    state->thread = std::jthread([alive = std::move(alive), onExit = std::move(onExit)](
                                     const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            if (!alive()) {
                onExit();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
    return std::unique_ptr<ProcessExitWatch>(new ProcessExitWatch(std::move(state)));
}

}  // namespace px::platform
