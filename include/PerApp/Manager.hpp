#pragma once

#include "LibUtils.hpp"
#include "Utils.hpp"
#include "Logger.hpp"
#include <unordered_map>
#include <fstream>
#include <cstring>
#include <string>

class PerAppManager {
private:
    static constexpr const char* perappPath = "/sdcard/Android/CTS/perapp_powermode.txt";
    static constexpr const char* modePath = "/sdcard/Android/CTS/mode.txt";

    Utils utils;
    Logger logger;

    std::unordered_map<std::string, std::string> rules;
    std::string defaultMode;
    bool loaded = false;

    /* 通配符匹配 */
    static bool matchWildcard(const char* pattern, const char* str) {
        const char* star = nullptr;
        const char* mark = nullptr;
        while (*str) {
            if (*pattern == '?') { pattern++; str++; }
            else if (*pattern == '*') { star = pattern++; mark = str; if (!*pattern) return true; }
            else if (*pattern == *str) { pattern++; str++; }
            else if (star) { pattern = star + 1; str = ++mark; }
            else return false;
        }
        while (*pattern == '*') pattern++;
        return *pattern == '\0';
    }

public:
    PerAppManager() = default;

    /* 加载配置 */
    void loadConfig() {
        rules.clear();
        defaultMode.clear();
        loaded = false;

        std::ifstream file(perappPath);
        if (!file.is_open()) return;

        std::string line;
        while (getline(file, line)) {
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            size_t end = line.find_last_not_of(" \t\r\n");
            line = line.substr(start, end - start + 1);

            if (line.empty() || line[0] == '#') continue;

            // * auto 默认模式
            if (line[0] == '*' && line.size() >= 2 && line[1] == ' ') {
                defaultMode = line.substr(2);
                continue;
            }

            // 包名 模式
            size_t sp = line.find(' ');
            if (sp == std::string::npos) continue;
            std::string pkg = line.substr(0, sp);
            std::string mode = line.substr(sp + 1);
            if (pkg.empty() || mode.empty()) continue;
            rules[pkg] = mode;
        }

        loaded = true;
        logger.Info("perapp: 已加载 %zu 条规则", rules.size());
    }

    /* 匹配包名 → 模式。返回 nullptr 表示无匹配 */
    const char* findMode(const char* packageName) {
        if (!loaded || !packageName) return nullptr;

        // 精确匹配
        auto it = rules.find(std::string(packageName));
        if (it != rules.end()) {
            if (it->second == "auto") return nullptr;
            return it->second.c_str();
        }

        // 通配符匹配
        for (const auto& entry : rules) {
            if (matchWildcard(entry.first.c_str(), packageName)) {
                if (entry.second == "auto") return nullptr;
                return entry.second.c_str();
            }
        }

        // 默认模式
        if (!defaultMode.empty()) {
            if (defaultMode == "auto") return nullptr;
            return defaultMode.c_str();
        }

        return nullptr;
    }

    void writeMode(const std::string& mode) {
        utils.WriteFile(modePath, mode.c_str());
    }

    bool isLoaded() const { return loaded; }
    const std::string& getDefaultMode() const { return defaultMode; }
};
