#pragma once

#include "LibUtils.hpp"
#include "Function.hpp"
#include "OnePlus.hpp"
#include "Feas/Feas.hpp"
#include "PerApp/Manager.hpp"
#include "PerApp/Detector.hpp"

class Schedule {
private:
    static constexpr const char* configPath = "/sdcard/Android/CTS/mode.txt";
    static constexpr const char* jsonPath = "/sdcard/Android/CTS/config.json";
    static constexpr const char* cpusetEventPath = "/dev/cpuset/top-app/cgroup.procs";
    static constexpr const char* onlinePath = "/sys/devices/system/cpu/cpu%d/online";
    static constexpr const char* SchedParamPath = "/sys/devices/system/cpu/cpufreq/policy%d/%s/%s";
    static constexpr const char* GovernorPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor";
    static constexpr const char* MinFreqPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq";
    static constexpr const char* MaxFreqPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq";

    std::vector<thread> threads;

    Function function;
    OnePlus oneplus;
    FeasManager feas;
    PerAppManager perapp;
    Detector detector;
    JsonConfig conf;
    Logger logger;
    Utils utils;

    bool cpuBoost = false;
    std::string lastTopApp;       // 上一次前台包名，perapp 去重
    std::string savedMode;
    bool inPerApp = false;
    std::string perAppPkg;

    char temp[256];
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
        FastSnprintf(temp, sizeof(temp), MinFreqPath, Policy);
        utils.FileWrite(temp, MinFreq);
        logger.Debug("CPU簇: %d 最小频率: %s", Policy, MinFreq.c_str());

        FastSnprintf(temp, sizeof(temp), MaxFreqPath, Policy);
        utils.FileWrite(temp, MaxFreq);
        logger.Debug("CPU簇: %d 最大频率: %s", Policy, MaxFreq.c_str());

        FastSnprintf(temp, sizeof(temp), GovernorPath, Policy);
        utils.FileWrite(temp, Governor);
        logger.Debug("CPU簇: %d 调速器: %s", Policy, Governor.c_str());
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
            FreqWriter(Policy::CpuPolicy[i], Performances::MinFreq[i], 
                    Performances::MaxFreq[i], Performances::CpuGovernor[i]);
        }
        function.FeasFunc(false);
    }

    void Reset() {
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], "0", "2147483647", 
                    function.checkQcom() ? "walt" : "sugov_ext");
        }
    }

    void applyOnePlusMode() {
        if (conf.mode == "fast") {
            for (int i = 0; i <= 3; i++) {
                if (Policy::CpuPolicy[i] == -1) continue;
                FreqWriter(Policy::CpuPolicy[i], "0", "2147483647",
                        oneplus.detectGovernor("", true));
            }
            oneplus.scheduleOfficialToggle(true);
        } else {
            Reset();
        }
    }

    void release() {
        if (oneplus.isHoraeOn() && conf.mode != "fast") {
            oneplus.scheduleOfficialToggle(false);
        }

        if (conf.mode.empty()) {
            logger.Warn("情景模式为空，跳过应用配置");
            return;
        }

        feas.disableAll();

        logger.Info("情景模式: %s 已启用", conf.mode.c_str());
        if (cpuBoost) {
            Boost();
            cpuBoost = false;
        } else if (oneplus.isActive(conf.mode)) {
            applyOnePlusMode();
        } else {
            Release();
        }

        if (conf.mode == "fast" && Feas::enable) {
            feas.enableAll();
        }
    }

    void online() {
        for (int i = 0; i <= 7; i++) {
            FastSnprintf(temp, sizeof(temp), onlinePath, i);
            utils.WriteInt(temp, Performances::Online[i]);
            logger.Debug("核心: %d %s", i, Performances::Online[i] ? "开启" : "关闭");
        }
    }

    void SchedParam() {
        for (int i = 0; i <= 3; i++) {
            for (int j = 1; j <= 12; j++) {
                if (Policy::CpuPolicy[i] == -1 || conf.schedParam[i].Name[j].empty()) continue;
                FastSnprintf(temp, sizeof(temp), SchedParamPath, Policy::CpuPolicy[i], Performances::CpuGovernor[i].c_str(), conf.schedParam[i].Name[j].c_str());
                utils.FileWrite(temp, conf.schedParam[i].Value[j].c_str());
                logger.Debug("CPU簇: %d 调速器参数: %d 名称: %s 值: %s", Policy::CpuPolicy[i], j, conf.schedParam[i].Name[j].c_str(), conf.schedParam[i].Value[j].c_str());
            }
        }
    }

    bool skipCtsIntervention() {
        return oneplus.isActive(conf.mode);
    }

    /* ========== Per-app ========== */

    std::atomic<bool> hasPerAppMode{false};
    std::string currentPerAppMode;
    std::string savedDefaultMode;
    std::string lastGameApp;
    std::string lastGameMode;

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

    void perappTriggerTask() {
        sleep(2);
        static constexpr const char* perappFile = "/sdcard/Android/CTS/perapp_powermode.txt";
        struct stat st{};
        stat(perappFile, &st);
        time_t lastMtime = st.st_mtime;
        while (true) {
            sleep(1);
            if (stat(perappFile, &st) < 0) continue;
            if (st.st_mtime == lastMtime) continue;
            lastMtime = st.st_mtime;
            logger.Info("perapp_powermode.txt 已修改，重载规则");
            printf("\nperapp_powermode.txt 已修改，重载规则\n");
            perapp.loadConfig();
        }
    }

    void perappPollTask() {
        sleep(3);
        while (true) {
            if (hasPerAppMode.load() && !lastGameApp.empty()) {
                if (!detector.isInForegroundCpuset(lastGameApp.c_str())) {
                    exitPerAppMode("不在前台cpuset");
                }
            }
            sleep(2);
        }
    }

    void configTriggerTask() {
        sleep(2);
        struct stat st{};
        stat(configPath, &st);
        time_t lastMtime = st.st_mtime;
        while (true) {
            sleep(1);
            if (stat(configPath, &st) < 0) continue;
            if (st.st_mtime == lastMtime) continue;
            lastMtime = st.st_mtime;
            conf.readConfig();
            release();
            if (!skipCtsIntervention()) {
                SchedParam();
                online();
            }
        }
    }

    void jsonTriggerTask() {
        sleep(2);
        struct stat st{};
        stat(jsonPath, &st);
        time_t lastMtime = st.st_mtime;
        while (true) {
            sleep(1);
            if (stat(jsonPath, &st) < 0) continue;
            if (st.st_mtime == lastMtime) continue;
            lastMtime = st.st_mtime;
            conf.readConfig();
            logger.setLogLevel(Meta::loglevel);
            if (!skipCtsIntervention()) {
                function.AllFunC();
            }

            if (OfficialMode::enable && !oneplus.checkOfficialGovAvailable()) {
                logger.Warn("[OnePlus] OfficialMode 已开启但未检测到 hmbird/scx 调速器，请关闭 OfficialMode");
            }
        }
    }

    void cpuSetTriggerTask() {
        constexpr int TRIGGER_BUF_SIZE = 8192;
        sleep(1);

        bool cpusetOk = (access(cpusetEventPath, F_OK) == 0);
        if (!cpusetOk) {
            logger.Warn("cpuset 不存在，LaunchBoost 和 per-app 不可用");
            while (true) sleep(3600);
        }

        while (true) {
            int fd = inotify_init();
            if (fd < 0) { sleep(5); continue; }

            int wd = inotify_add_watch(fd, cpusetEventPath, IN_MODIFY);
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

                if (LaunchBoost::enable && !cpuBoost) {
                    cpuBoost = true;
                }

                if (perapp.isLoaded()) {
                    if (stablePkg != lastTopApp) {
                        lastTopApp = stablePkg;
                        const char* mode = perapp.findMode(stablePkg.c_str());
                        bool fromWildcard = false;
                        if (mode) {
                            if (!hasPerAppMode.load() || mode != currentPerAppMode) {
                                hasPerAppMode.store(true);
                                currentPerAppMode = mode;
                                savedDefaultMode = conf.mode;
                                lastGameApp = stablePkg;
                                lastGameMode = mode;
                                perapp.writeMode(mode);
                                logger.Info("per-app: %s -> %s 已启用", stablePkg.c_str(), mode);
                            }
                        } else {
                            // 无匹配 → 小窗检测：检查上一个游戏是否还在前台cpuset
                            utils.sleep_ms(500);
                            bool gameInFg = false;
                            if (!lastGameApp.empty()) {
                                gameInFg = detector.isInForegroundCpuset(lastGameApp.c_str());
                            }
                            if (gameInFg) {
                                logger.Debug("per-app: %s 获焦但游戏 %s 仍在前台cpuset，保持画像", stablePkg.c_str(), lastGameApp.c_str());
                            } else if (hasPerAppMode.load()) {
                                exitPerAppMode("游戏不在前台cpuset");
                            }
                        }
                    } else if (hasPerAppMode.load() && !lastGameApp.empty() && stablePkg == lastGameApp) {
                        currentPerAppMode = lastGameMode;
                        perapp.writeMode(lastGameMode);
                        logger.Info("per-app: 游戏 %s 回到前台，恢复 %s", lastGameApp.c_str(), lastGameMode.c_str());
                    }
                }

                break;
            }

            inotify_rm_watch(fd, wd);
            close(fd);

            if (cpuBoost) {
                release();
            }
        }
    }

    void Init() {
        { char buf[256] = { 0 };
          if (!utils.popenRead("pidof LittleYouran_CTS", buf, sizeof(buf))) {
              logger.Error("进程检测失败");
              exit(-1);
          }
          auto ptr = strchr(buf, ' ');
          if (ptr) {
              char tmp[256];
              memcpy(tmp, buf, sizeof(tmp));
              tmp[sizeof(tmp)-1] = 0;
              for (char* t = strtok(tmp, " \t\n"); t; t = strtok(nullptr, " \t\n")) {
                  int pid = atoi(t);
                  if (pid > 0 && pid != (int)getpid()) {
                      printf("\nCTS 已在运行 (pid:%d), 正在终止\n", pid);
                      kill(pid, SIGTERM);
                      usleep(500000);
                      if (kill(pid, 0) == 0) { kill(pid, SIGKILL); usleep(500000); }
                      printf("旧进程已终止\n");
                      printf("重启成功\n");
                  }
              }
          }
        }

        logger.clear_log();
        conf.readConfig();
        perapp.loadConfig();
        logger.setLogLevel(Meta::loglevel);
        logger.Info("名称: %s", Meta::name.c_str());
        logger.Info("版本: %d", Meta::version);
        logger.Info("作者: %s", Meta::author.c_str());
        logger.Info("日志等级: %s", Meta::loglevel.c_str());

        logger.Info("[OnePlus] OfficialMode 开关: %s", OfficialMode::enable ? "开启" : "关闭");
        if (OfficialMode::enable) {
            if (oneplus.checkOfficialGovAvailable()) {
                logger.Info("[OnePlus] 检测到 hmbird/scx 调速器，风驰可用");
            } else {
                logger.Warn("[OnePlus] OfficialMode 已开启但未检测到 hmbird/scx 调速器，请关闭 OfficialMode");
            }
            oneplus.scheduleOfficialToggle(false);
        }

        if (Feas::enable && OfficialMode::enable) {
            logger.Error("[冲突] Feas 和 OfficialMode 不能同时开启！请关闭其中一个");
        }

        logger.Info("[Feas] Feas 开关: %s", Feas::enable ? "开启" : "关闭");
        if (Feas::enable && !feas.checkAvailable()) {
            logger.Warn("[Feas] Feas 已开启但设备无 Feas 接口，请关闭 Feas 功能");
        }

        function.AllFunC();
        release();
        if (!skipCtsIntervention()) {
            online();
            SchedParam();
        }
    }
};