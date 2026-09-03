#pragma once


#include "Logger.hpp"
#include "Utils.hpp"

#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_set>
#include <vector>

class XiaomiFeas {
public:
    static inline const std::unordered_set<std::string_view> kFeasGames = {
        "com.tencent.tmgp.cf",
        "com.tencent.jkchess",
        "com.miHoYo.Yuanshen",
        "com.tencent.tmgp.sgame",
        "com.tencent.lolm",
        "com.tencent.KiHan",
        "com.tencent.tmgp.sgamece",
        "com.tencent.tmgp.pubgmhd",
    };

    bool Init() {
        feasPath_ = nullptr;
        if (access(qcomFeas, F_OK) == 0) feasPath_ = qcomFeas;
        else if (access(mtkFeas, F_OK) == 0) feasPath_ = mtkFeas;
        return feasPath_ != nullptr;
    }

    bool supports(const std::string& processName) const {
        const std::string_view pkg(processName);
        const size_t suffix = pkg.find(':');
        const std::string_view base =
            suffix == std::string_view::npos ? pkg : pkg.substr(0, suffix);
        return kFeasGames.count(base) != 0;
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
        out.reserve(kFeasGames.size());
        for (std::string_view game : kFeasGames) out.emplace_back(game);
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