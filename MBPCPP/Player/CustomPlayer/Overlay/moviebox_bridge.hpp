#pragma once

#include "moviebox_protocol.hpp"
#include <algorithm>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

struct MovieBoxPlaybackState {
    std::int64_t revision = 0;
    double position = 0.0;
    double duration = 0.0;
    bool playing = false;
    bool ready = false;
    int volume = 100;
    bool muted = false;
};

bool MovieBoxBridgeStart(const std::wstring& pipeName);
void MovieBoxBridgeStop();
bool MovieBoxBridgeCloseSession(DWORD timeoutMs = 1200);
bool MovieBoxBridgeEnabled();
bool MovieBoxBridgeConnected();
void MovieBoxBridgeSetPlaybackState(const MovieBoxPlaybackState& state);
void MovieBoxBridgeQueueAction(const std::string& action, const std::string& id = {}, double value = 0.0);
bool MovieBoxBridgePopReply(mbp::PlaybackReply& reply);
std::string MovieBoxBridgeLastError();

#ifdef MOVIEBOX_BRIDGE_IMPLEMENTATION

namespace moviebox_bridge_detail {

struct PendingAction {
    std::string action;
    std::string id;
    double value = 0.0;
};

static std::atomic<bool> g_enabled{ false };
static std::atomic<bool> g_connected{ false };
static std::atomic<bool> g_stop{ false };
static std::atomic<bool> g_closeAck{ false };
static std::wstring g_pipeName;
static std::thread g_worker;
static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static std::mutex g_pipeMutex;
static std::mutex g_stateMutex;
static std::mutex g_queueMutex;
static std::condition_variable g_closeCv;
static MovieBoxPlaybackState g_state;
static std::deque<PendingAction> g_actions;
static std::deque<mbp::PlaybackReply> g_replies;
static std::string g_lastError;

static void SetError(const std::string& error) {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_lastError = error;
}

static std::string Win32Error(const char* prefix, DWORD code = GetLastError()) {
    return std::string(prefix) + " (Win32 " + std::to_string(code) + ")";
}

static void ClosePipeHandle() {
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    g_connected.store(false, std::memory_order_release);
}

static HANDLE CurrentPipe() {
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    return g_pipe;
}

static bool ConnectPipe() {
    const std::wstring path = L"\\\\.\\pipe\\" + g_pipeName;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (!g_stop.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        if (!WaitNamedPipeW(path.c_str(), 250)) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PIPE_BUSY || error == ERROR_SEM_TIMEOUT) continue;
            SetError(Win32Error("MovieBox bridge WaitNamedPipeW failed", error));
            return false;
        }

        HANDLE pipe = CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (pipe == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_BUSY || error == ERROR_FILE_NOT_FOUND) continue;
            SetError(Win32Error("MovieBox bridge CreateFileW failed", error));
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_pipeMutex);
            g_pipe = pipe;
        }
        g_connected.store(true, std::memory_order_release);
        return true;
    }

    if (!g_stop.load(std::memory_order_acquire)) SetError("Timed out connecting to MovieBoxPro playback bridge.");
    return false;
}

static bool WriteLine(const std::string& line) {
    HANDLE pipe = CurrentPipe();
    if (pipe == INVALID_HANDLE_VALUE) return false;
    std::string payload = line;
    payload.push_back('\n');

    const char* data = payload.data();
    std::size_t remaining = payload.size();
    while (remaining > 0 && !g_stop.load(std::memory_order_acquire)) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>((std::min<std::size_t>)(remaining, static_cast<std::size_t>(0x7fffffff)));
        if (!WriteFile(pipe, data, chunk, &written, nullptr) || written == 0) {
            SetError(Win32Error("MovieBox bridge WriteFile failed"));
            return false;
        }
        data += written;
        remaining -= written;
    }
    return remaining == 0;
}

static bool ReadLine(std::string& line) {
    line.clear();
    HANDLE pipe = CurrentPipe();
    if (pipe == INVALID_HANDLE_VALUE) return false;

    char c = 0;
    while (!g_stop.load(std::memory_order_acquire)) {
        DWORD read = 0;
        if (!ReadFile(pipe, &c, 1, &read, nullptr) || read == 0) {
            SetError(Win32Error("MovieBox bridge ReadFile failed"));
            return false;
        }
        if (c == '\n') return true;
        if (c != '\r') line.push_back(c);
        if (line.size() > 8u * 1024u * 1024u) {
            SetError("MovieBox bridge reply exceeded 8 MiB.");
            return false;
        }
    }
    return false;
}

static PendingAction TakeAction() {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_actions.empty()) return {};
    PendingAction action = std::move(g_actions.front());
    g_actions.pop_front();
    return action;
}

static bool HasQueuedActions() {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    return !g_actions.empty();
}

static MovieBoxPlaybackState PlaybackSnapshot() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_state;
}

static void PushReply(mbp::PlaybackReply reply) {
    const bool closed = reply.closed;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (!reply.error.empty()) g_lastError = reply.error;
        g_replies.push_back(std::move(reply));
    }
    if (closed) {
        g_closeAck.store(true, std::memory_order_release);
        g_closeCv.notify_all();
    }
}

static void WorkerMain() {
    if (!ConnectPipe()) return;

    auto lastMetadata = std::chrono::steady_clock::time_point{};
    while (!g_stop.load(std::memory_order_acquire)) {
        PendingAction pending = TakeAction();
        MovieBoxPlaybackState state = PlaybackSnapshot();

        mbp::PlaybackRequest request;
        request.action = pending.action.empty() ? "poll" : std::move(pending.action);
        request.id = std::move(pending.id);
        request.value = pending.value;
        request.revision = state.revision;
        request.position = state.position;
        request.duration = state.duration;
        request.playing = state.playing;
        request.ready = state.ready;
        request.volume = state.volume;
        request.muted = state.muted;

        const auto now = std::chrono::steady_clock::now();
        request.metadata = lastMetadata.time_since_epoch().count() == 0 || now - lastMetadata >= std::chrono::seconds(2);
        if (request.metadata) lastMetadata = now;

        if (!WriteLine(mbp::SerializePlaybackRequest(request))) break;
        std::string line;
        if (!ReadLine(line)) break;

        mbp::PlaybackReply reply;
        std::string parseError;
        if (!mbp::ParsePlaybackReply(line, reply, parseError)) {
            SetError("Invalid MovieBox bridge reply: " + parseError);
            break;
        }

        const bool closed = reply.closed;
        PushReply(std::move(reply));
        if (closed || request.action == "close") {
            if (request.action == "close") {
                g_closeAck.store(true, std::memory_order_release);
                g_closeCv.notify_all();
            }
            break;
        }

        if (!HasQueuedActions()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ClosePipeHandle();
}

} // namespace moviebox_bridge_detail

bool MovieBoxBridgeStart(const std::wstring& pipeName) {
    using namespace moviebox_bridge_detail;
    if (pipeName.empty()) return false;
    if (g_enabled.exchange(true, std::memory_order_acq_rel)) return true;
    g_pipeName = pipeName;
    g_stop.store(false, std::memory_order_release);
    g_closeAck.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_actions.clear();
        g_replies.clear();
        g_lastError.clear();
    }
    g_worker = std::thread(WorkerMain);
    return true;
}

void MovieBoxBridgeStop() {
    using namespace moviebox_bridge_detail;
    if (!g_enabled.exchange(false, std::memory_order_acq_rel)) return;
    g_stop.store(true, std::memory_order_release);

    if (g_worker.joinable()) {
        // The worker can be blocked in synchronous ReadFile/WriteFile. Cancel it
        // so player shutdown stays bounded even if MovieBoxPro disappeared.
        CancelSynchronousIo(g_worker.native_handle());
        ClosePipeHandle();
        g_worker.join();
    } else {
        ClosePipeHandle();
    }
}

bool MovieBoxBridgeCloseSession(DWORD timeoutMs) {
    using namespace moviebox_bridge_detail;
    if (!g_enabled.load(std::memory_order_acquire)) return true;
    if (!g_connected.load(std::memory_order_acquire)) return false;

    MovieBoxBridgeQueueAction("close");
    std::unique_lock<std::mutex> lock(g_queueMutex);
    return g_closeCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] {
        return g_closeAck.load(std::memory_order_acquire) || !g_connected.load(std::memory_order_acquire);
    });
}

bool MovieBoxBridgeEnabled() {
    return moviebox_bridge_detail::g_enabled.load(std::memory_order_acquire);
}

bool MovieBoxBridgeConnected() {
    return moviebox_bridge_detail::g_connected.load(std::memory_order_acquire);
}

void MovieBoxBridgeSetPlaybackState(const MovieBoxPlaybackState& state) {
    using namespace moviebox_bridge_detail;
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_state = state;
}

void MovieBoxBridgeQueueAction(const std::string& action, const std::string& id, double value) {
    using namespace moviebox_bridge_detail;
    if (!g_enabled.load(std::memory_order_acquire) || action.empty()) return;
    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_actions.push_back(PendingAction{ action, id, value });
}

bool MovieBoxBridgePopReply(mbp::PlaybackReply& reply) {
    using namespace moviebox_bridge_detail;
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_replies.empty()) return false;
    reply = std::move(g_replies.front());
    g_replies.pop_front();
    return true;
}

std::string MovieBoxBridgeLastError() {
    using namespace moviebox_bridge_detail;
    std::lock_guard<std::mutex> lock(g_queueMutex);
    return g_lastError;
}

#endif // MOVIEBOX_BRIDGE_IMPLEMENTATION
