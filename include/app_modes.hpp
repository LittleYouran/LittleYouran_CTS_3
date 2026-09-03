#pragma once

// ============================================================
// 应用单独模式模块（App Modes）：
//   AppModeConfig    —— 规则存储（app_modes.txt / mode.txt）
//   ScenePowerConfig —— 同步到 Scene 的 powercfg.xml（单向）
//   AppModeService   —— 服务入口：规则读写、文件监听、通知
// ============================================================

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>

#include "Utils.hpp"
#include "Logger.hpp"

class AppModeConfig {
public:
    using Rules = std::map<std::string, std::string>;

    static constexpr const char* directoryPath =
        "/storage/emulated/0/Android/CTS";
    static constexpr const char* configPath =
        "/storage/emulated/0/Android/CTS/app_modes.txt";
    static constexpr const char* modePath =
        "/storage/emulated/0/Android/CTS/mode.txt";

    static bool isSupportedMode(const std::string& mode) {
        return mode == "powersave" || mode == "balance" ||
               mode == "performance" || mode == "fast";
    }

    static bool isValidPackage(const std::string& package) {
        if (package.empty() || package.size() > 255) return false;
        if (package == "*") return true;
        return std::none_of(package.begin(), package.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        });
    }

    bool initialize() {
        Rules loaded;
        if (!readRules(configPath, loaded)) {
            loaded["*"] = readLegacyMode();
        }
        if (loaded.find("*") == loaded.end()) loaded["*"] = "balance";

        {
            std::lock_guard<std::mutex> lock(mutex_);
            rules_ = std::move(loaded);
        }
        return save(false);
    }

    bool reload() {
        Rules loaded;
        if (!readRules(configPath, loaded)) return false;
        if (loaded.find("*") == loaded.end()) {
            loaded["*"] = defaultMode();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (loaded == rules_) return false;
        rules_ = std::move(loaded);
        revision_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool setRule(const std::string& package, const std::string& mode) {
        if (!isValidPackage(package) || !isSupportedMode(mode)) return false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto current = rules_.find(package);
            if (current != rules_.end() && current->second == mode) return true;
            rules_[package] = mode;
        }
        return save(package == "*");
    }

    bool removeRule(const std::string& package) {
        if (package == "*" || !isValidPackage(package)) return false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (rules_.erase(package) == 0) return true;
        }
        return save(false);
    }

    bool replaceRules(Rules rules) {
        Rules normalized;
        for (auto& [package, mode] : rules) {
            if (isValidPackage(package) && isSupportedMode(mode)) {
                normalized.emplace(std::move(package), std::move(mode));
            }
        }
        if (normalized.find("*") == normalized.end()) {
            normalized["*"] = defaultMode();
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (normalized == rules_) return true;
            rules_ = std::move(normalized);
        }
        return save(false);
    }

    std::string resolve(const std::string& processName) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto exact = rules_.find(processName);
        if (exact != rules_.end()) return exact->second;

        const size_t suffix = processName.find(':');
        if (suffix != std::string::npos) {
            const auto base = rules_.find(processName.substr(0, suffix));
            if (base != rules_.end()) return base->second;
        }

        const auto fallback = rules_.find("*");
        return fallback == rules_.end() ? "balance" : fallback->second;
    }

    bool hasRule(const std::string& processName) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (rules_.find(processName) != rules_.end()) return true;
        const size_t suffix = processName.find(':');
        return suffix != std::string::npos &&
               rules_.find(processName.substr(0, suffix)) != rules_.end();
    }

    std::string defaultMode() const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = rules_.find("*");
        return it == rules_.end() ? "balance" : it->second;
    }

    Rules rules() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rules_;
    }

    unsigned long long revision() const {
        return revision_.load(std::memory_order_relaxed);
    }

    bool writeCurrentMode(const std::string& mode) const {
        if (!isSupportedMode(mode)) return false;
        // 与当前值一致时不重复写盘，避免触发自身 inotify 造成循环
        std::string current;
        {
            std::ifstream input(modePath);
            if (input) std::getline(input, current);
        }
        while (!current.empty() && std::isspace(static_cast<unsigned char>(current.back()))) {
            current.pop_back();
        }
        if (current == mode) return true;
        mkdir("/storage/emulated/0/Android", 0775);
        mkdir(directoryPath, 0775);
        return Utils::writeFileAtomic(modePath, mode + "\n");
    }


private:
    mutable std::mutex mutex_;
    Rules rules_;
    std::atomic<unsigned long long> revision_{0};

    static bool readRules(const char* path, Rules& rules) {
        std::ifstream input(path);
        if (!input) return false;

        std::string line;
        while (std::getline(input, line)) {
            line = Utils::trim(std::move(line));
            if (line.empty() || line[0] == '#') continue;

            std::istringstream stream(line);
            std::string package;
            std::string mode;
            std::string extra;
            if (!(stream >> package >> mode) || (stream >> extra)) continue;
            if (isValidPackage(package) && isSupportedMode(mode)) {
                rules[package] = mode;
            }
        }
        return !rules.empty();
    }

    std::string readLegacyMode() const {
        std::ifstream input(modePath);
        std::string mode;
        if (input) std::getline(input, mode);
        mode = Utils::trim(std::move(mode));
        return isSupportedMode(mode) ? mode : "balance";
    }

    bool save(const bool writeModeFile) {
        mkdir("/storage/emulated/0/Android", 0775);
        mkdir(directoryPath, 0775);

        Rules snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = rules_;
        }

        std::string content;
        const auto fallback = snapshot.find("*");
        const std::string mode = fallback == snapshot.end() ? "balance" : fallback->second;
        content += "* " + mode + '\n';
        for (const auto& [package, ruleMode] : snapshot) {
            if (package == "*") continue;
            content += package + " " + ruleMode + '\n';
        }

        if (!Utils::writeFileAtomic(configPath, content)) return false;
        if (writeModeFile && !Utils::writeFileAtomic(modePath, mode + "\n")) return false;
        revision_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
};

class ScenePowerConfig {
public:
    static constexpr const char* directoryPath =
        "/data/user/0/com.omarea.vtools/shared_prefs";
    static constexpr const char* configPath =
        "/data/user/0/com.omarea.vtools/shared_prefs/powercfg.xml";

    bool available() const {
        return access(configPath, F_OK) == 0;
    }

    bool loadRules(AppModeConfig::Rules& rules) const {
        const std::string xml = readFile();
        if (xml.empty()) return false;

        rules.clear();
        forEachString(xml, [&](const Entry& entry) {
            if (AppModeConfig::isValidPackage(entry.name) &&
                AppModeConfig::isSupportedMode(entry.value)) {
                rules[entry.name] = entry.value;
            }
        });
        return !rules.empty();
    }

    bool syncRules(const AppModeConfig::Rules& rules, const bool refreshRunning = false) const {
        if (access(directoryPath, F_OK) != 0) return false;

        struct stat original{};
        const bool hadFile = stat(configPath, &original) == 0;
        std::string xml = hadFile ? readFile() :
            std::string("<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n</map>\n");
        if (xml.empty()) return false;

        std::string merged;
        merged.reserve(xml.size() + rules.size() * 80);
        std::unordered_set<std::string> written;
        size_t copied = 0;

        forEachString(xml, [&](const Entry& entry) {
            merged.append(xml, copied, entry.begin - copied);
            const auto desired = rules.find(entry.name);
            if (desired != rules.end()) {
                appendEntry(merged, desired->first, desired->second);
                written.insert(desired->first);
            } else if (!AppModeConfig::isSupportedMode(entry.value)) {
                merged.append(xml, entry.begin, entry.end - entry.begin);
            }
            copied = entry.end;
        });

        const size_t mapEnd = xml.find("</map>", copied);
        if (mapEnd == std::string::npos) return false;
        merged.append(xml, copied, mapEnd - copied);
        if (!merged.empty() && merged.back() != '\n') merged.push_back('\n');
        for (const auto& [package, mode] : rules) {
            if (written.find(package) != written.end()) continue;
            appendEntry(merged, package, mode);
            merged.push_back('\n');
        }
        merged.append(xml, mapEnd, xml.size() - mapEnd);
        if (merged == xml) {
            if (refreshRunning) refreshSceneIfRunning();
            return true;
        }

        if (!Utils::writeFileAtomic(configPath, merged, hadFile ? &original : nullptr)) return false;

        if (refreshRunning) refreshSceneIfRunning();

        return true;
    }

private:
    struct Entry {
        size_t begin;
        size_t end;
        std::string name;
        std::string value;
    };

    template <typename Callback>
    static void forEachString(const std::string& xml, Callback callback) {
        size_t cursor = 0;
        while ((cursor = xml.find("<string", cursor)) != std::string::npos) {
            const size_t tagEnd = xml.find('>', cursor + 7);
            if (tagEnd == std::string::npos) break;
            const size_t close = xml.find("</string>", tagEnd + 1);
            if (close == std::string::npos) break;

            const size_t nameKey = xml.find("name=", cursor + 7);
            if (nameKey == std::string::npos || nameKey > tagEnd) {
                cursor = close + 9;
                continue;
            }
            const size_t quotePos = nameKey + 5;
            if (quotePos >= tagEnd || (xml[quotePos] != '\"' && xml[quotePos] != '\'')) {
                cursor = close + 9;
                continue;
            }
            const char quote = xml[quotePos];
            const size_t nameEnd = xml.find(quote, quotePos + 1);
            if (nameEnd == std::string::npos || nameEnd > tagEnd) {
                cursor = close + 9;
                continue;
            }

            callback(Entry{cursor, close + 9,
                           unescape(xml.substr(quotePos + 1, nameEnd - quotePos - 1)),
                           unescape(xml.substr(tagEnd + 1, close - tagEnd - 1))});
            cursor = close + 9;
        }
    }

    static std::string readFile() {
        std::ifstream input(configPath, std::ios::binary);
        if (!input) return {};
        return std::string(std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>());
    }

    static std::string escape(const std::string& value) {
        std::string escaped;
        escaped.reserve(value.size());
        for (char ch : value) {
            switch (ch) {
                case '&': escaped += "&amp;"; break;
                case '<': escaped += "&lt;"; break;
                case '>': escaped += "&gt;"; break;
                case '\"': escaped += "&quot;"; break;
                case '\'': escaped += "&apos;"; break;
                default: escaped.push_back(ch); break;
            }
        }
        return escaped;
    }

    static std::string unescape(std::string value) {
        const struct Replacement { const char* encoded; const char* decoded; } replacements[] = {
            {"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"},
            {"&gt;", ">"}, {"&amp;", "&"},
        };
        for (const auto& replacement : replacements) {
            size_t pos = 0;
            while ((pos = value.find(replacement.encoded, pos)) != std::string::npos) {
                value.replace(pos, std::strlen(replacement.encoded), replacement.decoded);
                pos += std::strlen(replacement.decoded);
            }
        }
        return value;
    }

    static void appendEntry(std::string& xml, const std::string& package,
                            const std::string& mode) {
        xml += "<string name=\"";
        xml += escape(package);
        xml += "\">";
        xml += escape(mode);
        xml += "</string>";
    }

    static void refreshSceneIfRunning() {
        Utils utils;
        char pids[128] = {0};
        if (utils.popenRead("pidof com.omarea.vtools", pids, sizeof(pids) - 1) == 0) {
            return;
        }
        // 后台静默重启：仅结束 Scene 进程使其重新冷加载 powercfg.xml。
        // 若 Scene 常驻(START_STICKY)已启用会自动在后台复活，不会跳到前台；
        // 未启用常驻则 Scene 保持停止，用户下次打开时会读取到新配置。
        utils.exec("pkill -f com.omarea.vtools");
    }
};

class AppModeService {
public:
    using ChangeCallback = std::function<void()>;

    bool initialize() {
        if (!config_.initialize()) {
            logger_.Error("应用模式配置初始化失败");
            return false;
        }

        // Scene 只作为同步目标，不反向导入其已有配置。
        if (scene_.available()) scene_.syncRules(config_.rules());
        return true;
    }

    void start(ChangeCallback callback) {
        callback_ = std::move(callback);
        running_.store(true, std::memory_order_relaxed);
        std::thread(&AppModeService::watchLocalFiles, this).detach();
    }

    bool setRule(const std::string& package, const std::string& mode) {
        std::lock_guard<std::mutex> lock(syncMutex_);
        if (!config_.setRule(package, mode)) return false;
        if (scene_.available()) scene_.syncRules(config_.rules(), true);
        notifyChanged();
        return true;
    }

    bool removeRule(const std::string& package) {
        std::lock_guard<std::mutex> lock(syncMutex_);
        if (!config_.removeRule(package)) return false;
        if (scene_.available()) scene_.syncRules(config_.rules(), true);
        notifyChanged();
        return true;
    }

    std::string resolve(const std::string& package) const {
        return config_.resolve(package);
    }

    std::string defaultMode() const {
        return config_.defaultMode();
    }

    bool hasRule(const std::string& package) const {
        return config_.hasRule(package);
    }

    bool writeCurrentMode(const std::string& mode) const {
        return config_.writeCurrentMode(mode);
    }

    AppModeConfig::Rules rules() const {
        return config_.rules();
    }

    bool sceneAvailable() const {
        return scene_.available();
    }

    unsigned long long revision() const {
        return config_.revision();
    }

private:
    AppModeConfig config_;
    ScenePowerConfig scene_;
    Logger logger_;
    ChangeCallback callback_;
    std::atomic<bool> running_{false};
    std::mutex syncMutex_;

    void notifyChanged() {
        if (callback_) callback_();
    }

    static std::string readModeFile() {
        std::ifstream input(AppModeConfig::modePath);
        std::string mode;
        if (input) std::getline(input, mode);
        while (!mode.empty() && std::isspace(static_cast<unsigned char>(mode.back()))) {
            mode.pop_back();
        }
        return mode;
    }

    void handleLocalChange(bool appConfigChanged) {
        std::lock_guard<std::mutex> lock(syncMutex_);
        const bool changed = appConfigChanged && config_.reload();
        if (!changed) return;
        if (scene_.available()) scene_.syncRules(config_.rules());
        notifyChanged();
    }

    void watchLocalFiles() {
        const int fd = inotify_init1(IN_CLOEXEC);
        if (fd < 0) return;
        const int wd = inotify_add_watch(fd, AppModeConfig::directoryPath,
            IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
        if (wd < 0) {
            close(fd);
            return;
        }

        char buffer[4096];
        while (running_.load(std::memory_order_relaxed)) {
            const ssize_t length = read(fd, buffer, sizeof(buffer));
            if (length <= 0) continue;

            bool appChanged = false;
            bool modeChanged = false;
            for (const char* cursor = buffer; cursor < buffer + length;) {
                const auto* event = reinterpret_cast<const inotify_event*>(cursor);
                if (event->wd == wd && event->len > 0) {
                    appChanged |= std::strcmp(event->name, "app_modes.txt") == 0;
                    modeChanged |= std::strcmp(event->name, "mode.txt") == 0;
                }
                cursor += sizeof(inotify_event) + event->len;
            }
            if (appChanged) handleLocalChange(true);
            else if (modeChanged) notifyChanged();
        }
        close(fd);
    }

};