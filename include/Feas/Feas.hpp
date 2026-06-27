#pragma once

#include "Utils.hpp"
#include "Logger.hpp"
#include "Config.hpp"

 // Feas 接口路径
class FeasManager {
private:
    static constexpr const char* qcomFeas  = "/sys/module/perfmgr/parameters/perfmgr_enable";
    static constexpr const char* qcomFeas2 = "/sys/module/perfmgr_policy/parameters/perfmgr_enable";
    static constexpr const char* mtkFeas   = "/sys/module/mtk_fpsgo/parameters/perfmgr_enable";

    Utils utils;
    Logger logger;

public:
    FeasManager() = default;

    // 检测 Feas 接口
    bool checkAvailable() const {
        return access(qcomFeas, F_OK) == 0 ||
               access(qcomFeas2, F_OK) == 0 ||
               access(mtkFeas, F_OK) == 0;
    }

    // 关闭 Feas 接口
    void disableAll() {
        if (access(qcomFeas, F_OK) == 0) {
            utils.FileWrite(qcomFeas, "0");
        }
        if (access(qcomFeas2, F_OK) == 0) {
            utils.FileWrite(qcomFeas2, "0");
        }
        if (access(mtkFeas, F_OK) == 0) {
            utils.FileWrite(mtkFeas, "0");
        }
        logger.Debug("Feas 已关闭");
    }
有 
    // 开启 Feas 接口
    void enableAll() {
        if (access(qcomFeas, F_OK) == 0) {
            utils.FileWrite(qcomFeas, "1");
        }
        if (access(qcomFeas2, F_OK) == 0) {
            utils.FileWrite(qcomFeas2, "1");
        }
        if (access(mtkFeas, F_OK) == 0) {
            utils.FileWrite(mtkFeas, "1");
        }
        logger.Info("Feas 已开启");
    }
};
