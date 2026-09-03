#pragma once

#include "Logger.hpp"
#include "Utils.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <unordered_set>

class ForegroundAppMonitor {
public:
    using Callback = std::function<void(const std::string&)>;
    // 判定“受关注应用”（例如配置了单独模式的游戏）：主屏是它时，小窗/浮层变化不切换模式
    using TrackedFn = std::function<bool(const std::string&)>;

    void start(Callback callback, TrackedFn tracked = {}) {
        callback_ = std::move(callback);
        trackedFn_ = std::move(tracked);
        running_.store(true, std::memory_order_relaxed);
        std::thread(&ForegroundAppMonitor::monitorTask, this).detach();
    }

    std::string currentPackage() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentPackage_;
    }

private:
    static constexpr const char* topAppProcs =
        "/dev/cpuset/top-app/cgroup.procs";
    static constexpr const char* topAppDirectory =
        "/dev/cpuset/top-app";

    // 小窗/系统浮层等服务永远不应作为“前台主应用”参与模式判定
    static bool isIgnoredShell(const std::string& process) {
        static constexpr const char* kIgnoredPrefixes[] = {
            "com.coloros.assistantscreen",
            "com.coloros.smartsidebar",
            "com.android.systemui",
            "com.coloros.screenrecorder",
        };
        for (const char* prefix : kIgnoredPrefixes) {
            if (process.rfind(prefix, 0) == 0) return true;
        }
        return false;
    }

    Utils utils_;
    Logger logger_;
    Callback callback_;
    TrackedFn trackedFn_;
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    std::unordered_set<int> topAppPids_;
    std::string currentPackage_;

    static bool looksLikePackage(const std::string& process) {
        if (process.empty() || process.find('.') == std::string::npos ||
            process.find('/') != std::string::npos ||
            process.find(' ') != std::string::npos) {
            return false;
        }
        return std::isalpha(static_cast<unsigned char>(process[0])) != 0;
    }

    std::unordered_set<int> readPidSet() {
        std::unordered_set<int> pids;
        char content[8192] = {0};
        const size_t length = utils_.readString(topAppProcs, content, sizeof(content) - 1);
        const char* cursor = content;
        const char* end = content + length;
        while (cursor < end) {
            while (cursor < end && std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
            if (cursor >= end) break;
            char* numberEnd = nullptr;
            const long pid = std::strtol(cursor, &numberEnd, 10);
            if (numberEnd == cursor) break;
            if (pid > 0) pids.insert(static_cast<int>(pid));
            cursor = numberEnd;
        }
        return pids;
    }

    struct Candidate {
        std::string process;
        bool mainProcess = false;
        bool tracked = false;
        int pid = -1;
    };

    // 从 top-app 进程集挑选前台主应用：
    //  1) 忽略小窗/系统壳
    //  2) 优先“受关注应用”（配置了单独模式的主屏游戏），避免被小窗内容应用顶掉
    //  3) 同优先级下取主进程(无 ':')、pid 更大者
    std::string detectPackage(const std::unordered_set<int>& pids) {
        Candidate bestTracked;
        Candidate bestNormal;
        for (const int pid : pids) {
            char path[64];
            FastSnprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
            char command[512] = {0};
            const size_t length = utils_.readString(path, command, sizeof(command) - 1);
            if (length == 0) continue;

            std::string process(command, strnlen(command, length));
            if (!looksLikePackage(process) || isIgnoredShell(process)) continue;

            Candidate cand;
            cand.process = process;
            cand.mainProcess = process.find(':') == std::string::npos;
            cand.tracked = trackedFn_ ? trackedFn_(process) : false;
            cand.pid = pid;

            Candidate& slot = cand.tracked ? bestTracked : bestNormal;
            if (slot.process.empty() ||
                (cand.mainProcess && !slot.mainProcess) ||
                (cand.mainProcess == slot.mainProcess && cand.pid > slot.pid)) {
                slot = cand;
            }
        }
        return bestTracked.process.empty() ? bestNormal.process : bestTracked.process;
    }

    bool packagePresent(const std::unordered_set<int>& pids,
                       const std::string& package) {
        if (package.empty()) return false;
        for (const int pid : pids) {
            char path[64];
            FastSnprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
            char command[512] = {0};
            const size_t length = utils_.readString(path, command, sizeof(command) - 1);
            if (length == 0) continue;
            const std::string process(command, strnlen(command, length));
            if (process == package) return true;
            const size_t suffix = process.find(':');
            if (suffix != std::string::npos && process.substr(0, suffix) == package) {
                return true;
            }
        }
        return false;
    }

    void publishIfChanged() {
        const auto newPids = readPidSet();
        std::unordered_set<int> added;
        for (const int pid : newPids) {
            if (topAppPids_.find(pid) == topAppPids_.end()) added.insert(pid);
        }

        std::string current;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current = currentPackage_;
        }

        // 主屏仍是“受关注应用”（例如游戏）时：小窗/浮层进进出出不切换模式，
        // 直到它真正离开 top-app（游戏退出）。
        if (!current.empty() && packagePresent(newPids, current) &&
            trackedFn_ && trackedFn_(current)) {
            topAppPids_ = newPids;
            return;
        }

        // cgroup transitions commonly emit an empty or removal-only snapshot.
        // Keep the current app until a genuinely new foreground process appears.
        if (added.empty() && !current.empty() && packagePresent(newPids, current)) {
            topAppPids_ = newPids;
            return;
        }

        const std::string package = detectPackage(added.empty() ? newPids : added);
        topAppPids_ = newPids;
        if (package.empty() || package == current) return;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (package == currentPackage_) return;
            currentPackage_ = package;
        }
        if (callback_) callback_(package);
    }

    void monitorTask() {
        publishIfChanged();
        const int fd = inotify_init1(IN_CLOEXEC);
        if (fd < 0) return;
        const int wd = inotify_add_watch(fd, topAppProcs,
            IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB);
        const int dirWd = inotify_add_watch(fd, topAppDirectory,
            IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_MOVED_TO | IN_CREATE);
        if (wd < 0 && dirWd < 0) {
            close(fd);
            return;
        }

        char buffer[4096];
        while (running_.load(std::memory_order_relaxed)) {
            const ssize_t length = read(fd, buffer, sizeof(buffer));
            if (length > 0) publishIfChanged();
        }
        close(fd);
    }
};
