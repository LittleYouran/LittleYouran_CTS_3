#pragma once

// ============================================================
// OPPO / OnePlus 风驰适配（完整）
//   OplusPackages —— 从 ColorOS 游戏数据库读取包名
//   OnePlus       —— 风驰引擎：调速器探测、horae 开关、包匹配
// ============================================================

#include "PackageDb.hpp"
#include "Logger.hpp"
#include "Utils.hpp"

#include <cstring>
#include <string>
#include <unistd.h>
#include <unordered_set>
#include <vector>

class OplusPackages {
private:
    static constexpr const char* databasePath1 =
        "/data/data/com.oplus.cosa/databases/db_game_database";
    static constexpr const char* databasePath2 =
        "/data/user_de/0/com.oplus.cosa/databases/db_game_database";

public:
    static bool databaseAvailable() {
        return access(databasePath1, F_OK) == 0 || access(databasePath2, F_OK) == 0;
    }

    bool load(std::vector<std::string>& packages, Utils& utils, Logger& logger) const {
        const char* database = nullptr;
        if (access(databasePath1, F_OK) == 0) database = databasePath1;
        else if (access(databasePath2, F_OK) == 0) database = databasePath2;
        if (database == nullptr) return false;

        std::vector<char> output;
        if (!PackageDb::query(database, "SELECT package_name FROM PackageConfigBean;",
                              utils, output)) {
            return false;
        }

        packages.clear();
        std::unordered_set<std::string> seen;
        const char* begin = output.data();
        const char* end = begin + output.size();
        while (begin < end) {
            const char* newline = static_cast<const char*>(memchr(begin, '\n', end - begin));
            const char* lineEnd = newline ? newline : end;
            while (lineEnd > begin && (lineEnd[-1] == '\r' || lineEnd[-1] == ' ' || lineEnd[-1] == '\t')) {
                --lineEnd;
            }

            if (lineEnd > begin) {
                std::string package(begin, lineEnd);
                if (package.rfind("com.oplus.", 0) != 0 &&
                    package.rfind("oplus.cosa.", 0) != 0 &&
                    seen.insert(package).second) {
                    packages.emplace_back(std::move(package));
                }
            }

            if (!newline) break;
            begin = newline + 1;
        }

        if (packages.empty()) return false;
        logger.Info("从 Oplus 检测到包名 (%u 个)",
                    static_cast<unsigned>(packages.size()));
        return true;
    }
};

class OnePlus {
public:
    bool Init() {
        governor_ = detectGovernor();
        if (governor_.empty()) return false;

        std::vector<std::string> packages;
        if (!packageSource_.load(packages, utils_, logger_)) return false;
        packages_.clear();
        packages_.insert(packages.begin(), packages.end());
        return !packages_.empty();
    }

    bool supports(const std::string& processName) const {
        if (packages_.find(processName) != packages_.end()) return true;
        const size_t suffix = processName.find(':');
        return suffix != std::string::npos &&
               packages_.find(processName.substr(0, suffix)) != packages_.end();
    }

    bool available() const { return !governor_.empty(); }

    void setActive(bool enable) {
        if (active_ == enable) return;
        active_ = enable;

        char command[128];
        FastSnprintf(command, sizeof(command),
            "resetprop -n persist.sys.horae.enable %d", enable ? 1 : 0);
        utils_.exec(command);
        utils_.exec(enable ? "start horae" : "stop horae");
        logger_.Info(enable ? "风驰 horae 已开启" : "风驰 horae 已关闭");
    }

    bool active() const { return active_; }

    const std::string& getGovernor() const {
        return governor_;
    }

    std::vector<std::string> packages() const {
        return std::vector<std::string>(packages_.begin(), packages_.end());
    }

private:
    static constexpr const char* availableGovernorsPath =
        "/sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors";

    Utils utils_;
    Logger logger_;
    OplusPackages packageSource_;
    std::unordered_set<std::string> packages_;
    std::string governor_;
    bool active_ = false;

    std::string detectGovernor() {
        char governors[512] = {0};
        const size_t length = utils_.readString(availableGovernorsPath, governors,
                                                sizeof(governors) - 1);
        if (length == 0) return {};
        if (std::strstr(governors, "hmbird")) return "hmbird";
        if (std::strstr(governors, "scx")) return "scx";
        return {};
    }
};