#pragma once

#include "LibUtils.hpp"
#include "Function.hpp"
#include "WebSocket.hpp"
#include "onePlus.hpp"

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
    OnePlus oneoppo;
    WebSocketServer wsServer;
    JsonConfig conf;
    Logger logger;
    Utils utils;

    bool cpuBoost = false;
    
    char temp[256];
public:
    Schedule& operator=(Schedule&&) = delete;

    Schedule() {
        Init();
        threads.emplace_back(thread(&Schedule::configTriggerTask, this));
        threads.emplace_back(thread(&Schedule::jsonTriggerTask, this));
        threads.emplace_back(thread(&Schedule::cpuSetTriggerTask, this));
        oneoppo.Start();
        wsServer.Start();
    }

    void FreqWriter(const int Policy, const string_t& MinFreq, const string_t& MaxFreq, const string_t& Governor) {
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
        }
        utils.sleep_ms(LaunchBoost::boost_rate_limit_ms);
    }

    void Release() {
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], Performances::MinFreq[i], Performances::MaxFreq[i], Performances::CpuGovernor[i]);
        }
        function.FeasFunc(false);
    }

    // 风驰: 频率不限制 (先切一次 performance 再切目标调速器)
    void applyOnePlusMode() {
        const char* gov = oneoppo.getGovernor();
        logger.Info("风驰增强: 调速器 %s 频率不限制", gov);
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], "0", "2147483647", "performance");
        }
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], "0", "2147483647", gov);
        }
        GameMode::active = true;
    }

    void release() {
        logger.Info("情景模式: %s 已启用", conf.mode.c_str());
        if (cpuBoost) {
            Boost();
            cpuBoost = false;
            Release();
        } else if (conf.mode == "fast" && GameMode::active) {
            // 风驰的 fast: 有 hmbird/scx 走频率不限制(不按配置文件), 否则按配置文件
            if (oneoppo.getGovernor() != nullptr) {
                applyOnePlusMode();
            } else {
                Release();
            }
        } else {
            // 非风驰 (无论是否 fast) 及官方模式: 按配置文件 (自动恢复官方调速器)
            Release();
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

    void configTriggerTask() {
        sleep(2);
        while (true) {
            utils.InotifyMain(configPath, IN_CLOSE_WRITE | IN_MODIFY);
            std::lock_guard<std::mutex> lock(Config::applyMutex);
            if (!conf.readConfig()) continue;
            release();
            SchedParam();
            online();
        }
    }

    void jsonTriggerTask() {
        sleep(2);
        while (true) {
            utils.InotifyMain(jsonPath, IN_CLOSE_WRITE | IN_MODIFY);
            std::lock_guard<std::mutex> lock(Config::applyMutex);
            conf.readConfig();
            logger.setLogLevel(Meta::loglevel);
            function.AllFunC();
        }
    }

    void cpuSetTriggerTask() {
        if (!LaunchBoost::enable) return;
        
        sleep(1);
        const int fd = inotify_init();      
        if (fd < 0) {
            logger.Error("无法初始化 inotify");
            exit(-1);
        }

        const int wd = inotify_add_watch(fd, cpusetEventPath, IN_MODIFY);
        if (wd < 0) {
            logger.Error("inotify 初始化 %s 失败", cpusetEventPath);
            exit(-1);
        }

        char buf[8192];

        logger.Info("监听顶层应用切换事件成功");

        while (read(fd, buf, sizeof(buf)) > 0) {
            {
                std::lock_guard<std::mutex> lock(Config::applyMutex);
                cpuBoost = true;
                release();
            }
            logger.Debug("前台进程已切换 已触发LaunchBoost");
            utils.sleep_ms(500); // 防抖s
        }

        inotify_rm_watch(fd, wd);
        close(fd);
    }
    

    void Init() {
        char buf[256] = { 0 };
        // 进程名: Littleyouran (修复: pidof 无输出(无重复进程)时 popenRead 返回 0, 不应视为检测失败)
        const size_t runLen = utils.popenRead("pidof Littleyouran", buf, sizeof(buf) - 1);
        if (runLen > 0) {
            buf[runLen] = 0;
            auto ptr = strchr(buf, ' ');

            if (ptr) {
                logger.Error("CTS调度已经在运行(pid: %s), 当前进程(pid:%d)即将退出", buf, getpid());
                printf("\n!!! \n!!! CTS调度已经在运行(pid: %s), 当前进程(pid:%d)即将退出 \n!!!\n\n", buf, getpid());
                exit(-1);
            }
        }

        logger.clear_log();
        conf.readConfig();
        logger.setLogLevel(Meta::loglevel);
        logger.Info("名称: %s", Meta::name.empty() ? "CpuTurboScheduler" : Meta::name.c_str());
        logger.Info("版本: %d", Meta::version);
        logger.Info("作者: %s", Meta::author.empty() ? "Unknown" : Meta::author.c_str());
        logger.Info("日志等级: %s", Meta::loglevel.empty() ? "INFO" : Meta::loglevel.c_str());
        function.AllFunC();
        oneoppo.Init();
        release();
        online();
        SchedParam();
    }
};
