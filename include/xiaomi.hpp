#pragma once

#include "Logger.hpp"
#include "Utils.hpp"

#include <cstddef>
#include <string>
#include <unistd.h>
#include <vector>

class XiaomiFeas {
public:
    static constexpr const char* kFeasGames[] = {
        "com.tencent.tmgp.cf",
        "com.tencent.jkchess",
        "com.miHoYo.Yuanshen",
        "com.tencent.tmgp.sgame",
        "com.tencent.lolm",
        "com.tencent.KiHan",
        "com.tencent.tmgp.sgamece",
        "com.tencent.tmgp.pubgmhd",
    };
    static constexpr size_t kFeasGameCount = sizeof(kFeasGames) / sizeof(kFeasGames[0]);

    bool Init() {
        feasPath_ = nullptr;
        if (access(qcomFeas, F_OK) == 0) feasPath_ = qcomFeas;
        else if (access(mtkFeas, F_OK) == 0) feasPath_ = mtkFeas;
        return feasPath_ != nullptr;
    }

    bool supports(const std::string& processName) const {
        const size_t suffix = processName.find(':');
        const std::string base =
            suffix == std::string::npos ? processName : processName.substr(0, suffix);
        for (size_t i = 0; i < kFeasGameCount; ++i) {
            if (base == kFeasGames[i]) return true;
        }
        return false;
    }

    void setActive(bool enable) {
        if (active_ == enable || feasPath_ == nullptr) return;
        active_ = enable;
        utils_.FileWrite(feasPath_, enable ? "1" : "0");
        logger_.Info(enable ? "FEAS 已开启" : "FEAS 已关闭");
    }

    bool active() const { return active_; }

    std::vector<std::string> packages() const {
        std::vector<std::string> out;
        if (feasPath_ == nullptr) return out;
        out.reserve(kFeasGameCount);
        for (size_t i = 0; i < kFeasGameCount; ++i) out.emplace_back(kFeasGames[i]);
        return out;
    }

private:
    static constexpr const char* qcomFeas =
        "/sys/module/perfmgr/parameters/perfmgr_enable";
    static constexpr const char* mtkFeas =
        "/sys/module/mtk_fpsgo/parameters/perfmgr_enable";

    Utils utils_;
    Logger logger_;
    const char* feasPath_ = nullptr;
    bool active_ = false;
};
