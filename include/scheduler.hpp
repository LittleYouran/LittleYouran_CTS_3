#pragma once

#include "LibUtils.hpp"
#include "Function.hpp"
#include <sys/inotify.h>
#include <atomic>
#include <mutex>
#include <cstring>

class Schedule {
private:
    static constexpr const char* ctsDir = "/sdcard/Android/CTS";
    static constexpr const char* configPath = "/sdcard/Android/CTS/mode.txt";
    static constexpr const char* jsonPath = "/sdcard/Android/CTS/config.json";
    static constexpr const char* perappPath = "/sdcard/Android/CTS/perapp_powermode.txt";
    static constexpr const char* cpusetEventPath = "/dev/cpuset/top-app/cgroup.procs";
    static constexpr const char* onlinePath = "/sys/devices/system/cpu/cpu%d/online";
    static constexpr const char* SchedParamPath = "/sys/devices/system/cpu/cpufreq/policy%d/%s/%s";
    static constexpr const char* GovernorPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor";
    static constexpr const char* MinFreqPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq";
    static constexpr const char* MaxFreqPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq";
    static constexpr const char* MsmPerfMaxFreq = "/sys/module/msm_performance/parameters/cpu_max_freq";
    static constexpr const char* MsmPerfMinFreq = "/sys/module/msm_performance/parameters/cpu_min_freq";

    std::vector<thread> threads;
    std::mutex mtx;

    Function function;
    JsonConfig conf;
    Logger logger;
    Utils utils;

    std::atomic<bool> cpuBoost{false};
    std::atomic<bool> hasPerAppMode{false};
    std::string lastConfigMode;
    std::string lastReleasedMode;
    std::string lastTopApp;
    std::string lastLoggedMode;
    std::string currentPerAppMode;
    std::string savedDefaultMode;
    std::string lastGameApp;
    std::string lastGameMode;
    std::mutex releaseMutex;

    std::thread horaeDelayThread;
    std::mutex horaeMutex;
    char temp[256];
    string_t lastMinFreq[4];
    string_t lastMaxFreq[4];
    string_t lastGovernor[4];
public:
    Schedule& operator=(Schedule&&) = delete;

    Schedule() {
        Init();
        threads.emplace_back(thread(&Schedule::configTriggerTask, this));
        threads.emplace_back(thread(&Schedule::jsonTriggerTask, this));
        threads.emplace_back(thread(&Schedule::perappTriggerTask, this));
        threads.emplace_back(thread(&Schedule::cpuSetTriggerTask, this));
        threads.emplace_back(thread(&Schedule::perappPollTask, this));
    }

    void FreqWriter(const int Policy, const string_t MinFreq, const string_t MaxFreq, const string_t Governor) {
        std::lock_guard<std::mutex> lock(mtx);
        const char* gov = (Governor == "auto") ? function.detectGovernor("") : Governor.c_str();
        int idx = -1;
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == Policy) { idx = i; break; }
        }
        if (idx >= 0) {
            bool changed = false;
            if (lastGovernor[idx] != gov) { lastGovernor[idx] = gov; changed = true; }
            if (lastMaxFreq[idx] != MaxFreq) { lastMaxFreq[idx] = MaxFreq; changed = true; }
            if (lastMinFreq[idx] != MinFreq) { lastMinFreq[idx] = MinFreq; changed = true; }
            if (!changed) return;
        }
        FastSnprintf(temp, sizeof(temp), GovernorPath, Policy);
        utils.FileWrite(temp, gov);
        chmod(temp, 0444);
        FastSnprintf(temp, sizeof(temp), MaxFreqPath, Policy);
        utils.FileWrite(temp, MaxFreq);
        chmod(temp, 0444);
        FastSnprintf(temp, sizeof(temp), MinFreqPath, Policy);
        utils.FileWrite(temp, MinFreq);
        chmod(temp, 0444);
    }

    void clearFreqCache() {
        for (int i = 0; i <= 3; i++) {
            lastMinFreq[i].clear();
            lastMaxFreq[i].clear();
            lastGovernor[i].clear();
        }
    }

    void Boost() {
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], Performances::MinFreq[i],
                    LaunchBoost::BoostFreq[i], Performances::CpuGovernor[i]);
            utils.sleep_ms(LaunchBoost::boost_rate_limit_ms);
        }
    }

    void Release() {
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], Performances::MinFreq[i], Performances::MaxFreq[i], Performances::CpuGovernor[i]);
        }
        utils.FileWrite("/proc/sys/kernel/sched_util_clamp_min", "0");
        utils.FileWrite("/proc/sys/kernel/sched_util_clamp_max", "1024");
        if (Feas::enable) {
            function.DisableMiFeas();
        }
    }

    void Reset() {
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            const char* gov = function.checkQcom() ? "walt" : "sugov_ext";
            FreqWriter(Policy::CpuPolicy[i], "0", "2147483647", gov);
        }
        utils.FileWrite("/proc/sys/kernel/sched_util_clamp_min", "0");
        utils.FileWrite("/proc/sys/kernel/sched_util_clamp_max", "1024");
    }

    void ResetFast() {
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            const char* gov = function.detectGovernor("");
            FreqWriter(Policy::CpuPolicy[i], "0", "2147483647", gov);
        }
        utils.FileWrite("/proc/sys/kernel/sched_util_clamp_min", "0");
        utils.FileWrite("/proc/sys/kernel/sched_util_clamp_max", "1024");
    }

    void ResetMiFeas() {
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            const char* gov = function.detectGovernor("");
            FreqWriter(Policy::CpuPolicy[i], "0", "2147483647", gov);
        }
        utils.FileWrite("/proc/sys/kernel/sched_util_clamp_min", "0");
        utils.FileWrite("/proc/sys/kernel/sched_util_clamp_max", "1024");
        function.EnableMiFeas();
    }

    void scheduleOfficialToggle(bool enable) {
        std::lock_guard<std::mutex> lock(horaeMutex);
        if (horaeDelayThread.joinable()) horaeDelayThread.detach();
        horaeDelayThread = std::thread([this, enable]() {
            sleep(3);
            if (enable) {
                system("setprop persist.sys.horae.enable 1");
                char result[32] = {0};
                utils.popenRead("getprop persist.sys.horae.enable", result, sizeof(result));
                if (result[0] == '1') logger.Info("horae 已开启");
                else logger.Warn("horae 开启失败: %s", result);
            } else {
                system("setprop persist.sys.horae.enable 0");
                char result[32] = {0};
                utils.popenRead("getprop persist.sys.horae.enable", result, sizeof(result));
                if (result[0] == '0' || result[0] == 0) logger.Info("horae 已关闭");
                else logger.Warn("horae 关闭失败: %s", result);
            }
        });
    }

    void disableMsmPerformance() {
        if (access(MsmPerfMaxFreq, F_OK) == 0) {
            utils.FileWrite(MsmPerfMaxFreq, "0");
            utils.FileWrite(MsmPerfMinFreq, "0");
        }
    }

    void release() {
        std::lock_guard<std::mutex> lock(releaseMutex);

        if (cpuBoost.load()) {
            cpuBoost.store(false);
            clearFreqCache();
            disableMsmPerformance();
            Boost();
            lastReleasedMode = conf.mode;
            lastLoggedMode = conf.mode;
        } else if (OfficialMode::enable && conf.mode == "fast") {
            if (lastReleasedMode == conf.mode) return;
            lastReleasedMode = conf.mode;
            clearFreqCache();
            disableMsmPerformance();
            scheduleOfficialToggle(true);
            ResetFast();
            logger.Info("情景模式: fast 已启用，风驰已启用");
            lastLoggedMode = conf.mode;
        } else if (OfficialMode::enable && conf.mode == "performance") {
            if (lastReleasedMode == conf.mode) return;
            lastReleasedMode = conf.mode;
            clearFreqCache();
            disableMsmPerformance();
            Reset();
            logger.Info("情景模式: performance 已启用，风驰已启用");
            lastLoggedMode = conf.mode;
        } else if (Feas::enable && conf.mode == "fast") {
            if (lastReleasedMode == conf.mode) return;
            lastReleasedMode = conf.mode;
            clearFreqCache();
            disableMsmPerformance();
            Release();
            function.EnableMiFeas();
            logger.Info("Feas 已开启");
            logger.Info("情景模式: fast 已启用");
            lastLoggedMode = conf.mode;
        } else {
            if (lastReleasedMode == conf.mode) return;
            lastReleasedMode = conf.mode;
            clearFreqCache();
            disableMsmPerformance();
            if (lastLoggedMode == "fast" && OfficialMode::enable) {
                scheduleOfficialToggle(false);
            }
            if (lastLoggedMode == "fast" && Feas::enable) {
                function.DisableMiFeas();
                logger.Info("Feas 已关闭");
            }
            Release();
            logger.Info("情景模式: %s 已启用", conf.mode.c_str());
            lastLoggedMode = conf.mode;
        }
    }

    static constexpr const char* affectedCpusPath = "/sys/devices/system/cpu/cpufreq/policy%d/affected_cpus";

    void waitForAffectedCpus(int policy) {
        char path[128];
        FastSnprintf(path, sizeof(path), affectedCpusPath, policy);
        for (int retry = 0; retry < 50; retry++) {
            char buf[64] = {0};
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                close(fd);
                if (n > 0) {
                    buf[n] = 0;
                    bool hasOnline = false;
                    for (char* p = buf; *p; p++) {
                        if (*p >= '0' && *p <= '9') { hasOnline = true; break; }
                    }
                    if (hasOnline) return;
                }
            }
            utils.sleep_ms(20);
        }
    }

    void online() {
        for (int i = 0; i <= 7; i++) {
            FastSnprintf(temp, sizeof(temp), onlinePath, i);
            if (Performances::Online[i] == 1) {
                utils.WriteInt(temp, 1);
            }
        }
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            waitForAffectedCpus(Policy::CpuPolicy[i]);
        }
        for (int i = 0; i <= 7; i++) {
            FastSnprintf(temp, sizeof(temp), onlinePath, i);
            if (Performances::Online[i] == 0) {
                utils.WriteInt(temp, 0);
            }
        }
    }

    void SchedParam() {
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            for (int j = 1; j <= 12; j++) {
                if (conf.schedParam[i].Name[j].empty()) continue;
                if (conf.schedParam[i].Name[j] == "sched_util_clamp_min" ||
                    conf.schedParam[i].Name[j] == "sched_util_clamp_max") continue;
                FastSnprintf(temp, sizeof(temp), SchedParamPath, Policy::CpuPolicy[i], Performances::CpuGovernor[i].c_str(), conf.schedParam[i].Name[j].c_str());
                utils.FileWrite(temp, conf.schedParam[i].Value[j].c_str());
            }
        }
    }

    void configTriggerTask() {
        sleep(2);
        while (true) {
            utils.InotifyMain(ctsDir, IN_CLOSE_WRITE, "mode.txt");
            conf.readConfig();
            release();
            SchedParam();
            online();
        }
    }

    void jsonTriggerTask() {
        sleep(2);
        while (true) {
            utils.InotifyMain(ctsDir, IN_CLOSE_WRITE, "config.json");
            conf.readConfig();
            logger.setLogLevel(Meta::loglevel);
            function.AllFunC();
            logger.Info("配置文件更新已生效");

            if (Feas::enable && !function.checkFeasAvailable()) {
            logger.Warn("Feas 已开启但设备无 Feas 接口，请关闭 Feas 功能");
            }
            if (OfficialMode::enable && !function.checkOfficialGovAvailable()) {
            logger.Warn("OfficialMode 已开启但未检测到 hmbird/scx 调速器，请关闭 OfficialMode");
            }
        }
    }

    void perappTriggerTask() {
        sleep(2);
        while (true) {
            utils.InotifyMain(ctsDir, IN_CLOSE_WRITE, "perapp_powermode.txt");
            conf.loadPerApp();
            logger.Info("perapp_powermode.txt 文件变化，已重新加载");
            if (hasPerAppMode.load() && !lastGameApp.empty()) {
                bool fromWildcard = false;
                const char* mode = conf.findPerAppMode(lastGameApp.c_str(), &fromWildcard);
                if (!mode) {
                    exitPerAppMode("规则已删除");
                }
            }
        }
    }

    void exitPerAppMode(const char* reason) {
        hasPerAppMode.store(false);
        currentPerAppMode.clear();
        lastGameApp.clear();
        lastGameMode.clear();
        if (!savedDefaultMode.empty()) {
            utils.WriteFile(configPath, savedDefaultMode.c_str());
            logger.Info("per-app: %s，恢复默认模式 %s", reason, savedDefaultMode.c_str());
            savedDefaultMode.clear();
        }
    }

    void perappPollTask() {
        sleep(3);
        while (true) {
            if (hasPerAppMode.load() && !lastGameApp.empty()) {
                bool gameInFg = utils.isAppInForegroundCpuset(lastGameApp.c_str());
                if (!gameInFg) {
                    exitPerAppMode("游戏不在前台cpuset");
                }
            }
            sleep(2);
        }
    }

    void cpuSetTriggerTask() {
        constexpr int TRIGGER_BUF_SIZE = 8192;
        sleep(1);

        bool cpusetOk = (access(cpusetEventPath, F_OK) == 0);
        if (!cpusetOk) {
            logger.Warn("cpuset路径不存在，LaunchBoost和per-app检测不可用");
            while (true) sleep(3600);
        }

        while (true) {
            const int fd = inotify_init();
            if (fd < 0) { sleep(5); continue; }

            const int wd = inotify_add_watch(fd, cpusetEventPath, IN_MODIFY);
            if (wd < 0) { close(fd); sleep(5); continue; }

            char buf[TRIGGER_BUF_SIZE];
            while (read(fd, buf, TRIGGER_BUF_SIZE) > 0) {
                std::string stablePkg;
                for (int retry = 0; retry < 5; retry++) {
                    utils.sleep_ms(300);
                    char topBuf[256] = {0};
                    size_t topLen = utils.popenRead("dumpsys window | grep mCurrentFocus", topBuf, sizeof(topBuf));
                    if (topLen > 0) {
                        const char* u0 = strstr(topBuf, "u0");
                        if (u0) {
                            const char* ptr = u0 + 3;
                            const char* end_pos = strchr(ptr, '/');
                            if (end_pos && end_pos > ptr) {
                                std::string pkg(ptr, end_pos - ptr);
                                if (pkg == stablePkg && !pkg.empty()) break;
                                stablePkg = pkg;
                            }
                        }
                    }
                }

                if (stablePkg.empty()) { break; }

                if (LaunchBoost::enable && !cpuBoost.load()) {
                    cpuBoost.store(true);
                }

                if (PerApp::count > 0) {
                    if (stablePkg != lastTopApp) {
                        lastTopApp = stablePkg;
                        bool fromWildcard = false;
                        const char* mode = conf.findPerAppMode(stablePkg.c_str(), &fromWildcard);
                        if (mode && !fromWildcard) {
                            if (!hasPerAppMode.load() || strcmp(mode, currentPerAppMode.c_str()) != 0) {
                                hasPerAppMode.store(true);
                                currentPerAppMode = mode;
                                savedDefaultMode = conf.mode;
                                lastGameApp = stablePkg;
                                lastGameMode = mode;
                                utils.WriteFile(configPath, mode);
                                logger.Info("per-app: %s -> %s 已启用", stablePkg.c_str(), mode);
                            }
                        } else if ((mode && fromWildcard && hasPerAppMode.load()) || (!mode && hasPerAppMode.load())) {
                            utils.sleep_ms(500);
                            bool gameInFg = false;
                            if (!lastGameApp.empty()) {
                                gameInFg = utils.isAppInForegroundCpuset(lastGameApp.c_str());
                            }
                            if (gameInFg) {
                                logger.Debug("per-app: %s 获焦但游戏 %s 仍在前台cpuset，保持游戏画像", stablePkg.c_str(), lastGameApp.c_str());
                            } else {
                                exitPerAppMode("游戏不在前台cpuset");
                            }
                        } else if (mode && fromWildcard && !hasPerAppMode.load()) {
                            utils.WriteFile(configPath, mode);
                            logger.Info("per-app: %s -> %s (通配符匹配)", stablePkg.c_str(), mode);
                        }
                    } else if (hasPerAppMode.load() && !lastGameApp.empty() && stablePkg == lastGameApp) {
                        currentPerAppMode = lastGameMode;
                        utils.WriteFile(configPath, lastGameMode.c_str());
                        logger.Info("per-app: 游戏 %s 回到前台，恢复 %s", lastGameApp.c_str(), lastGameMode.c_str());
                    }
                }

                break;
            }

            inotify_rm_watch(fd, wd);
            close(fd);

            if (cpuBoost.load()) {
                release();
            }
        }
    }

    void Init() {
        { char pidBuf[256] = {0};
          size_t pidLen = utils.popenRead("pidof LittleYouran_CTS", pidBuf, sizeof(pidBuf));
          if (pidLen > 0) { char tmp[256]; memcpy(tmp, pidBuf, pidLen); tmp[pidLen] = 0;
              for (char* t = strtok(tmp, " \t\n"); t; t = strtok(nullptr, " \t\n")) {
                  int pid = atoi(t);
                  if (pid > 0 && pid != (int)getpid()) { printf("\nCTS 已在运行 (pid:%d), 正在终止\n", pid); kill(pid, SIGTERM); usleep(500000); if (kill(pid, 0) == 0) { kill(pid, SIGKILL); usleep(500000); } printf("旧进程已终止\n"); printf("重启成功\n"); }
              }
          }
        }
        logger.clear_log();
        conf.readConfig();
        conf.loadPerApp();
        logger.setLogLevel(Meta::loglevel);
        logger.Info("名称: %s", Meta::name.c_str());
        logger.Info("版本: %d", Meta::version);
        logger.Info("作者: %s", Meta::author.c_str());
        logger.Info("日志等级: %s", Meta::loglevel.c_str());

        if (Feas::enable && OfficialMode::enable) {
            logger.Error("Feas 和 OfficialMode 不能同时开启!");
            printf("\n!!! Feas 和 OfficialMode 不能同时开启 !!!\n");
            exit(-1);
        }

        logger.Info("小米Feas 开关: %s", Feas::enable ? "开启" : "关闭");
        logger.Info("OfficialMode风驰 开关: %s", OfficialMode::enable ? "开启" : "关闭");

        if (Feas::enable && !function.checkFeasAvailable()) {
            logger.Warn("Feas 已开启但设备无 Feas 接口，请关闭 Feas 功能");
        }

        if (OfficialMode::enable && !function.checkOfficialGovAvailable()) {
            logger.Warn("OfficialMode 已开启但未检测到 hmbird/scx 调速器，请关闭 OfficialMode");
        }

        function.AllFunC();
        release();
        online();
        SchedParam();
        lastConfigMode = conf.mode;
    }
};
