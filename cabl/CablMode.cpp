/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include <android-base/properties.h>
#include <android/log.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr char kMode[] = "persist.vendor.display.cabl.mode";
constexpr char kApplied[] = "vendor.display.cabl.applied";
constexpr char kState[] = "vendor.display.cabl.state";
constexpr char kClient[] = "/vendor/bin/ppd";
const auto kCallTimeout = 3s;
const auto kOverallTimeout = 40s;
Clock::time_point deadline;
std::string requested;
bool superseded = false;
bool allowNoop = false;
bool drainPrevious = false;

void Log(const std::string& text) {
    __android_log_print(ANDROID_LOG_INFO, "cabl-mode", "%s", text.c_str());
}
std::string Current() {
    return android::base::GetProperty(kMode, "off");
}
bool Latest() {
    if (Current() == requested) return true;
    superseded = true;
    return false;
}
bool Valid(const std::string& value) {
    return value == "off" || value == "Low" || value == "Medium" || value == "High";
}
std::string Trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \r\t\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \r\t\n") - first + 1);
}
struct Reply {
    bool completed = false;
    std::string text;
    std::vector<std::string> tokens;
    bool Has(const std::string& token) const {
        return completed && std::find(tokens.begin(), tokens.end(), token) != tokens.end();
    }
};

// The existing vendor client can print a failed daemon reply and still exit 0.
// Bound every child, parse its response, and separately verify state after commands.
Reply Call(const std::string& command) {
    Reply reply;
    if (!Latest() || Clock::now() >= deadline) return reply;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return reply;
    const pid_t pid = fork();
    if (pid == -1) {
        close(fds[0]); close(fds[1]);
        return reply;
    }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0 || dup2(fds[1], STDERR_FILENO) < 0) _exit(126);
        close(fds[1]);
        execl(kClient, "ppd", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    const int flags = fcntl(fds[0], F_GETFL);
    bool failed = flags < 0 || fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) < 0;
    bool exited = false;
    bool eof = false;
    int status = 0;
    const auto end = std::min(deadline, Clock::now() + kCallTimeout);
    while (!failed && !(exited && eof)) {
        char buffer[1024];
        while (true) {
            const ssize_t size = read(fds[0], buffer, sizeof(buffer));
            if (size > 0) {
                reply.text.append(buffer, static_cast<size_t>(size));
                if (reply.text.size() > 16384) { failed = true; break; }
            } else if (size == 0) {
                eof = true; break;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                failed = true; break;
            }
        }
        if (!exited) {
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid) exited = true;
            else if (result < 0 && errno != EINTR) { failed = true; break; }
        }
        if (exited && eof) break;
        if (Clock::now() >= end || !Latest()) { failed = true; break; }
        pollfd fd = {fds[0], POLLIN, 0};
        poll(&fd, 1, 100);
    }
    close(fds[0]);
    if (!exited) {
        kill(pid, SIGKILL);
        // Do not replace a command timeout with an unbounded wait for a task
        // stuck in the kernel. Init also owns/reaps this service's process group.
        const auto reapEnd = Clock::now() + 500ms;
        while (Clock::now() < reapEnd) {
            const pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid || (result < 0 && errno != EINTR)) break;
            std::this_thread::sleep_for(10ms);
        }
    }
    reply.completed = !failed && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    size_t offset = 0;
    while (offset < reply.text.size()) {
        size_t endline = reply.text.find('\n', offset);
        if (endline == std::string::npos) endline = reply.text.size();
        auto token = Trim(reply.text.substr(offset, endline - offset));
        constexpr char prefix[] = "postproc_info:";
        if (token.compare(0, sizeof(prefix) - 1, prefix) == 0) {
            token = Trim(token.substr(sizeof(prefix) - 1));
        }
        if (!token.empty()) reply.tokens.push_back(token);
        offset = endline + 1;
    }
    if (!reply.completed || reply.Has("Failure") || reply.Has("Command Failure")) {
        Log(command + " failed, timed out, or was superseded");
    }
    return reply;
}
bool Command(const std::string& command) {
    auto reply = Call(command);
    return reply.Has("Success") && !reply.Has("Failure") && !reply.Has("Command Failure");
}
bool WaitValue(const std::string& query, const std::string& expected,
               std::chrono::seconds timeout, std::chrono::milliseconds settle = 0ms) {
    const auto notBefore = Clock::now() + settle;
    const auto end = std::min(deadline, Clock::now() + timeout);
    Reply reply;
    while (Clock::now() < end && Latest()) {
        reply = Call(query);
        if (reply.Has(expected) && !reply.Has("Failure") &&
                !reply.Has("Command Failure") && Clock::now() >= notBefore) return true;
        std::this_thread::sleep_for(250ms);
    }
    if (Latest()) {
        // Log only a bounded, printable vendor-client response, once per failed
        // wait. Successful polling remains quiet.
        auto detail = reply.text.substr(0, 240);
        for (auto& c : detail) {
            if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
        }
        Log(query + " did not confirm " + expected + "; client_completed=" +
                (reply.completed ? "true" : "false") + "; last_reply=" + detail);
    }
    return false;
}
bool ConfigureStrength() {
    if (!Latest() || !Command("cabl:on") ||
        !Command("cabl:set " + requested) ||
        !WaitValue("cabl:get", requested, 8s)) return false;
    const auto status = Call("cabl:status");
    if (status.Has("Failure") || status.Has("Command Failure")) return false;
    if (status.Has("running")) return true;
    if (status.Has("stopped")) {
        // The daemon retains and accepts strength changes while processing is
        // paused, for example below its calibrated brightness threshold. The
        // strength readback above confirms configuration, not active dimming.
        Log("Strength " + requested + " confirmed; CABL processing is currently stopped");
        return true;
    }
    Log("Strength readback matched, but CABL processing status was not recognized");
    return false;
}
bool Start() {
    if (ConfigureStrength()) return true;
    if (!Latest() || Clock::now() >= deadline) return false;
    // Recover one failed configuration through off, preserving the overall
    // deadline and cancellation. A verified paused strength needs no restart.
    Log("CABL configuration unconfirmed; normalizing through off before one retry");
    if (!Command("cabl:off") ||
        !WaitValue("cabl:status", "stopped", 8s, 2500ms)) return false;
    return Latest() && ConfigureStrength();
}
bool Apply() {
    if (!WaitValue("cabl:support", "Success", 10s)) return false;
    const auto status = Call("cabl:status");
    if (requested == "off") {
        // Stopped processing can mean an enabled feature paused at low
        // brightness. Always disarm it explicitly so it cannot resume later.
        return Command("cabl:off") &&
                WaitValue("cabl:status", "stopped", 8s, 2500ms);
    }
    if (allowNoop && status.Has("running") && Call("cabl:get").Has(requested)) return true;
    // Normalize before changing strength. In our device trial, stopping took
    // 1-2 seconds; do not race a new start against that asynchronous transition.
    if ((drainPrevious || !status.Has("stopped")) &&
        (!Command("cabl:off") || !WaitValue("cabl:status", "stopped", 8s,
                drainPrevious ? 2500ms : 0ms))) return false;
    return Start();
}
}  // namespace

int main() {
    requested = Current();
    const auto previousState = android::base::GetProperty(kState, "");
    allowNoop = previousState == "applied" &&
            android::base::GetProperty(kApplied, "") == requested;
    // A superseded helper may have queued an asynchronous off/on command.
    // An immediately matching query is not sufficient in that case: normalize
    // through off and wait before applying the new request.
    drainPrevious = previousState == "applying" || previousState == "error";
    deadline = Clock::now() + kOverallTimeout;
    if (!Valid(requested)) {
        android::base::SetProperty(kState, "error");
        Log("Invalid saved mode; leaving CABL unchanged");
        return 1;
    }
    if (!android::base::SetProperty(kState, "applying")) {
        Log("Cannot publish apply status; leaving CABL unchanged");
        return 1;
    }
    const bool applied = Apply();
    if (superseded || !Latest()) return 0;
    if (applied && android::base::SetProperty(kApplied, requested) &&
        android::base::SetProperty(kState, "applied")) {
        Log("Confirmed mode " + requested);
        return 0;
    }
    android::base::SetProperty(kState, "error");
    Log("Could not confirm mode " + requested);
    return 1;
}
