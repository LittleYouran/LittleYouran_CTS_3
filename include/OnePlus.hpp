#pragma once

#include "LibUtils.hpp"
#include "Utils.hpp"
#include "Logger.hpp"
#include "Config.hpp"

class OnePlus {
private:
    static constexpr const char* cpusetPath = "/dev/cpuset/";
    static constexpr const char* qcomGpuPath = "/sys/class/kgsl/kgsl-3d0/";
    static constexpr const char* easSchedPath = "/proc/sys/kernel/sched_energy_aware";

    Utils utils;
    Logger logger;

    std::thread horaeDelayThread;
    std::mutex horaeMutex;

    bool horaeActive = false;

    char temp[256];

public:
    OnePlus() = default;
    ~OnePlus() {
        if (horaeDelayThread.joinable())
            horaeDelayThread.detach();
    }

    bool isActive(const std::string& mode) const {
        return OfficialMode::enable && (mode == "fast" || mode == "performance");
    }

    /* 调度器检测*/

    bool checkOfficialGovAvailable() {
        char buf[512] = {0};
        int fd = open("/sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors", O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = 0;
                if (strstr(buf, "hmbird") || strstr(buf, "scx"))
                    return true;
            }
        }
        return false;
    }

    // oneplusMode=true: 优先 hmbird > scx（极速模式）
    // oneplusMode=false: 优先 walt > sugov_ext（性能模式）
    const char* detectGovernor(const char* configured, bool oneplusMode = false) {
        if (configured && configured[0])
            return configured;

        char buf[512] = {0};
        int fd = open("/sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors", O_RDONLY);
        if (fd < 0)
            return checkQcom() ? "walt" : "sugov_ext";

        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            return checkQcom() ? "walt" : "sugov_ext";

        buf[n] = 0;

        if (oneplusMode) {
            static const char* candidates[] = {"hmbird", "scx", "walt", "sugov_ext", "schedutil"};
            for (auto g : candidates) {
                if (strstr(buf, g)) {
                    logger.Debug("Detected governor: %s", g);
                    return g;
                }
            }
        } else {
            static const char* candidates[] = {"walt", "sugov_ext", "schedutil"};
            for (auto g : candidates) {
                if (strstr(buf, g)) {
                    logger.Debug("Detected governor: %s", g);
                    return g;
                }
            }
        }

        return checkQcom() ? "walt" : "sugov_ext";
    }

    /* Horae开关*/
    bool isHoraeOn() const { return horaeActive; }

    void scheduleOfficialToggle(bool enable) {
        std::lock_guard<std::mutex> lock(horaeMutex);
        if (horaeDelayThread.joinable())
            horaeDelayThread.detach();

        horaeActive = enable;

        horaeDelayThread = std::thread([this, enable]() {
            sleep(3);
            if (enable) {
                system("setprop persist.sys.horae.enable 1");
                char result[32] = {0};
                utils.popenRead("getprop persist.sys.horae.enable", result, sizeof(result));
                if (result[0] == '1')
                    logger.Info("horae 已开启");
                else
                    logger.Warn("horae 开启失败: %s", result);
            } else {
                system("setprop persist.sys.horae.enable 0");
                char result[32] = {0};
                utils.popenRead("getprop persist.sys.horae.enable", result, sizeof(result));
                if (result[0] == '0' || result[0] == 0)
                    logger.Info("horae 已关闭");
                else
                    logger.Warn("horae 关闭失败: %s", result);
            }
        });
    }

    bool checkQcom() const {
        return (!access(qcomGpuPath, F_OK));
    }

    bool checkEasSched() const {
        return (!access(easSchedPath, F_OK));
    }

    bool checkCpuset() const {
        return (!access(cpusetPath, F_OK));
    }
};
