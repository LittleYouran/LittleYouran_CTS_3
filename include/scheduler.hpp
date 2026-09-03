#pragma once

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <condition_variable>

#include "LibUtils.hpp"
#include "Function.hpp"
#include "app_modes.hpp"
#include "ForegroundAppMonitor.hpp"
#include "GamePackageFile.hpp"
#include "WebSocket.hpp"
#include "xiaomi.hpp"
#include "oppo.hpp"

class Schedule {
private:
    static constexpr const char* jsonPath = "/sdcard/Android/CTS/config.json";
    static constexpr const char* onlinePath = "/sys/devices/system/cpu/cpu%d/online";
    static constexpr const char* SchedParamPath = "/sys/devices/system/cpu/cpufreq/policy%d/%s/%s";
    static constexpr const char* GovernorPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor";
    static constexpr const char* MinFreqPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq";
    static constexpr const char* MaxFreqPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq";
    static constexpr const char* AvailableFreqPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_available_frequencies";
    static constexpr const char* CpuInfoMinFreqPath = "/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_min_freq";
    static constexpr const char* CpuInfoMaxFreqPath = "/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_max_freq";
    static constexpr const char* PpmPolicyStatusPath = "/proc/ppm/policy_status";
    static constexpr const char* PpmUserMinFreqPath = "/proc/ppm/policy/userlimit_min_cpu_freq";
    static constexpr const char* PpmUserMaxFreqPath = "/proc/ppm/policy/userlimit_max_cpu_freq";
    static constexpr const char* PpmHardMinFreqPath = "/proc/ppm/policy/hard_userlimit_min_cpu_freq";
    static constexpr const char* PpmHardMaxFreqPath = "/proc/ppm/policy/hard_userlimit_max_cpu_freq";
    static constexpr int PpmUserLimitPolicyIndex = 8;
    static constexpr int PpmHardLimitPolicyIndex = 7;
    static constexpr int PpmPolicyEnabled = 1;
    static constexpr int PpmPolicyDisabled = 0;
    static constexpr int PpmUnlimitedFrequency = -1;
    static constexpr size_t FrequencyBufferSize = 4096;

    std::vector<thread> threads;

    Function function;
    AppModeService appModes;
    ForegroundAppMonitor foregroundMonitor;
    OnePlus onePlus;
    XiaomiFeas xiaomiFeas;
    WebSocketServer wsServer;
    JsonConfig conf;
    Logger logger;
    Utils utils;

    std::string currentPackage;
    std::string effectiveMode;
    std::string lastReleasedMode;
    std::string lastReportedPackage;
    std::string lastReportedMode;
    bool lastAppliedHadExplicitRule = false;
    
    char temp[256];

    std::string readNode(const char* path) {
        char value[FrequencyBufferSize] = { 0 };
        const size_t length = utils.readString(path, value, sizeof(value) - 1);
        if (length == 0) return {};

        std::string result(value, length);
        while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back()))) {
            result.pop_back();
        }
        return result;
    }

    bool modeFileIsFast() const {
        return readModeFile() == "fast";
    }

    std::string readModeFile() const {
        std::ifstream input(AppModeConfig::modePath);
        std::string mode;
        if (!input || !std::getline(input, mode)) return {};
        while (!mode.empty() && std::isspace(static_cast<unsigned char>(mode.back()))) {
            mode.pop_back();
        }
        return AppModeConfig::isSupportedMode(mode) ? mode : std::string();
    }

    std::vector<long long> availableFrequencies(const int policy) {
        FastSnprintf(temp, sizeof(temp), AvailableFreqPath, policy);
        const std::string raw = readNode(temp);
        std::stringstream stream(raw);
        std::vector<long long> frequencies;
        long long frequency = 0;

        while (stream >> frequency) {
            if (frequency > 0) frequencies.push_back(frequency);
        }

        std::sort(frequencies.begin(), frequencies.end());
        frequencies.erase(std::unique(frequencies.begin(), frequencies.end()), frequencies.end());
        return frequencies;
    }

    bool parseFrequency(const std::string& text, long long& value) const {
        if (text.empty()) return false;

        char* end = nullptr;
        errno = 0;
        value = std::strtoll(text.c_str(), &end, 10);
        return errno != ERANGE && end != text.c_str() && *end == '\0';
    }

    std::string normalizeMinFrequency(const int policy, const string_t& requested) {
        const std::string requestedText = requested.c_str();
        long long target = 0;
        if (!parseFrequency(requestedText, target)) return requestedText;
        if (target <= 0) return requestedText;

        const std::vector<long long> frequencies = availableFrequencies(policy);
        if (!frequencies.empty()) {
            const auto lower = std::lower_bound(frequencies.begin(), frequencies.end(), target);
            return std::to_string(lower == frequencies.end() ? frequencies.back() : *lower);
        }

        FastSnprintf(temp, sizeof(temp), CpuInfoMinFreqPath, policy);
        const std::string hardwareMin = readNode(temp);
        long long hardwareValue = 0;
        if (parseFrequency(hardwareMin, hardwareValue) && hardwareValue > 0) {
            return std::to_string(std::max(target, hardwareValue));
        }
        return requestedText;
    }

    std::string normalizeMaxFrequency(const int policy, const string_t& requested) {
        const std::string requestedText = requested.c_str();
        long long target = 0;
        if (!parseFrequency(requestedText, target)) return requestedText;

        const std::vector<long long> frequencies = availableFrequencies(policy);
        if (frequencies.empty()) {
            FastSnprintf(temp, sizeof(temp), CpuInfoMaxFreqPath, policy);
            const std::string hardwareMax = readNode(temp);
            long long hardwareValue = 0;
            if (parseFrequency(hardwareMax, hardwareValue) && hardwareValue > 0) {
                return std::to_string(std::min(target, hardwareValue));
            }
            return requestedText;
        }

        const auto upper = std::upper_bound(frequencies.begin(), frequencies.end(), target);
        const long long effective = upper == frequencies.begin() ? frequencies.front() : *std::prev(upper);
        return std::to_string(effective);
    }

    bool writeNode(const char* path, const std::string& value, const char* label) {
        utils.FileWrite(path, value.c_str());
        (void)label;
        return true;
    }

    bool ppmUserLimitAvailable() const {
        return access(PpmPolicyStatusPath, F_OK) == 0 &&
               access(PpmUserMinFreqPath, F_OK) == 0 &&
               access(PpmUserMaxFreqPath, F_OK) == 0;
    }

    bool ppmHardLimitAvailable() const {
        return access(PpmHardMinFreqPath, F_OK) == 0 &&
               access(PpmHardMaxFreqPath, F_OK) == 0;
    }

    bool writePpmValue(const char* path, const char* value) {
        int fd = open(path, O_WRONLY | O_NONBLOCK);
        if (fd < 0) return false;

        const size_t length = Faststrlen(value);
        const bool written = write(fd, value, length) == static_cast<ssize_t>(length);
        close(fd);
        return written;
    }

    bool writePpmCommand(const char* path, const int first, const std::string& second) {
        char command[64];
        FastSnprintf(command, sizeof(command), "%d %s", first, second.c_str());
        return writePpmValue(path, command);
    }

    bool setPpmPolicy(const int policyIndex, const int state) {
        char command[32];
        FastSnprintf(command, sizeof(command), "%d %d", policyIndex, state);
        return writePpmValue(PpmPolicyStatusPath, command);
    }

    bool setPpmUserLimitPolicy(const int state) {
        return setPpmPolicy(PpmUserLimitPolicyIndex, state);
    }

    bool applyPpmFrequencyRange(const int cluster, const std::string& minFreq,
                                const std::string& maxFreq) {
        if (!ppmUserLimitAvailable()) return false;

        struct stat statusStat{};
        struct stat minStat{};
        struct stat maxStat{};
        struct stat hardMinStat{};
        struct stat hardMaxStat{};
        if (stat(PpmPolicyStatusPath, &statusStat) != 0 ||
            stat(PpmUserMinFreqPath, &minStat) != 0 ||
            stat(PpmUserMaxFreqPath, &maxStat) != 0) {
            return false;
        }

        const bool hardAvailable = ppmHardLimitAvailable();
        if (hardAvailable &&
            (stat(PpmHardMinFreqPath, &hardMinStat) != 0 ||
             stat(PpmHardMaxFreqPath, &hardMaxStat) != 0)) {
            return false;
        }

        constexpr mode_t writableMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
        const bool permissionsReady = chmod(PpmPolicyStatusPath, writableMode) == 0 &&
            chmod(PpmUserMinFreqPath, writableMode) == 0 &&
            chmod(PpmUserMaxFreqPath, writableMode) == 0 &&
            (!hardAvailable ||
             (chmod(PpmHardMinFreqPath, writableMode) == 0 &&
              chmod(PpmHardMaxFreqPath, writableMode) == 0));

        const std::string unlimited = std::to_string(PpmUnlimitedFrequency);
        const bool hardApplied = !hardAvailable ||
            (setPpmPolicy(PpmHardLimitPolicyIndex, PpmPolicyEnabled) &&
             writePpmCommand(PpmHardMaxFreqPath, cluster, unlimited) &&
             writePpmCommand(PpmHardMinFreqPath, cluster, minFreq) &&
             writePpmCommand(PpmHardMaxFreqPath, cluster, maxFreq));
        const bool applied = permissionsReady && hardApplied &&
            setPpmUserLimitPolicy(PpmPolicyEnabled) &&
            writePpmCommand(PpmUserMaxFreqPath, cluster, unlimited) &&
            writePpmCommand(PpmUserMinFreqPath, cluster, minFreq) &&
            writePpmCommand(PpmUserMaxFreqPath, cluster, maxFreq);

        if (!applied) {
            setPpmUserLimitPolicy(PpmPolicyDisabled);
            if (hardAvailable) setPpmPolicy(PpmHardLimitPolicyIndex, PpmPolicyDisabled);
            logger.Debug("PPM CPU簇 %d 频率范围写入失败，已回退 sysfs", cluster);
        }

        const bool permissionsRestored =
            chmod(PpmPolicyStatusPath, statusStat.st_mode & 07777) == 0 &&
            chmod(PpmUserMinFreqPath, minStat.st_mode & 07777) == 0 &&
            chmod(PpmUserMaxFreqPath, maxStat.st_mode & 07777) == 0 &&
            (!hardAvailable ||
             (chmod(PpmHardMinFreqPath, hardMinStat.st_mode & 07777) == 0 &&
              chmod(PpmHardMaxFreqPath, hardMaxStat.st_mode & 07777) == 0));
        if (!permissionsRestored) {
            logger.Debug("PPM 节点权限恢复失败");
        }
        return applied && permissionsRestored;
    }

    bool anotherInstanceRunning() {
        char buffer[256] = { 0 };
        const size_t length = utils.popenRead("pidof Littleyouran", buffer, sizeof(buffer) - 1);
        if (length == 0) return false;
        buffer[length] = '\0';

        std::stringstream stream(buffer);
        pid_t pid = 0;
        while (stream >> pid) {
            if (pid != getpid()) return true;
        }
        return false;
    }

public:
    Schedule& operator=(Schedule&&) = delete;

    Schedule() {
        logger.clear_log();
        Init();
        threads.emplace_back(thread(&Schedule::jsonTriggerTask, this));
        wsServer.Configure(&appModes,
            [this]() { return getCurrentPackage(); },
            [this]() { return getEffectiveMode(); });
        appModes.start([this]() {
            applyCurrentState(false, true);
            wsServer.NotifyStateChanged();
        });
        foregroundMonitor.start(
            [this](const std::string& package) {
                if (package.empty()) return;
                {
                    std::lock_guard<std::mutex> stateLock(stateMutex);
                    currentPackage = package;
                }
                applyCurrentState(true, false);
                wsServer.NotifyStateChanged();
            },
            [this](const std::string& package) { return appModes.hasRule(package); });
        wsServer.Start();
    }

    void FreqWriter(const int Policy, const int Cluster, const string_t& MinFreq,
                    const string_t& MaxFreq, const string_t& Governor) {
        const std::string effectiveMinFreq = normalizeMinFrequency(Policy, MinFreq);
        const std::string effectiveMaxFreq = normalizeMaxFrequency(Policy, MaxFreq);
        const bool ppmApplied = applyPpmFrequencyRange(Cluster, effectiveMinFreq, effectiveMaxFreq);

        FastSnprintf(temp, sizeof(temp), MinFreqPath, Policy);
        if (!ppmApplied) {
            writeNode(temp, effectiveMinFreq, "最小频率");
        }
        logger.Debug("CPU簇: %d 最小频率: %s (配置=%s)", Policy, effectiveMinFreq.c_str(), MinFreq.c_str());

        FastSnprintf(temp, sizeof(temp), MaxFreqPath, Policy);
        if (!ppmApplied) {
            writeNode(temp, effectiveMaxFreq, "最大频率");
        }
        logger.Debug("CPU簇: %d 最大频率: %s (配置=%s)", Policy, effectiveMaxFreq.c_str(), MaxFreq.c_str());

        FastSnprintf(temp, sizeof(temp), GovernorPath, Policy);
        writeNode(temp, Governor.c_str(), "调速器");
        logger.Debug("CPU簇: %d 调速器: %s", Policy, Governor.c_str());
    }

    void Boost() {
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], i, Performances::MinFreq[i], 
                    LaunchBoost::BoostFreq[i], Performances::CpuGovernor[i]);
        }
        utils.sleep_ms(LaunchBoost::boost_rate_limit_ms);
    }

    void Release() {
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], i, Performances::MinFreq[i], Performances::MaxFreq[i], Performances::CpuGovernor[i]);
        }
    }

    void applyOnePlusMode() {
        const std::string& gov = onePlus.getGovernor();
        if (gov.empty()) {
            logger.Warn("风驰增强: 调速器不可用，跳过");
            return;
        }
        logger.Info("风驰增强: 调速器 %s 频率不限制", gov.c_str());
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], i, "0", "2147483647", "performance");
        }
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], i, "0", "2147483647", gov.c_str());
        }
    }

    void release(bool launchBoost) {
        if (lastReleasedMode != conf.mode) {
            logger.Info("情景模式: %s 已启用", conf.mode.c_str());
            lastReleasedMode = conf.mode;
        }
        if (launchBoost && LaunchBoost::enable) {
            Boost();
            Release();
        } else {
            Release();
        }
    }

    void applyCurrentState(bool launchBoost, bool force) {
        std::lock_guard<std::mutex> lock(Config::applyMutex);

        std::string package;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            package = currentPackage;
        }

        const std::string configuredMode = appModes.resolve(package);
        const bool hasExplicitRule = appModes.hasRule(package);
        const bool leavingExplicitRule = !hasExplicitRule &&
            package != lastAppliedPackage && lastAppliedHadExplicitRule;

        std::string mode;
        if (hasExplicitRule) {
            mode = configuredMode;
        } else if (leavingExplicitRule) {
            mode = appModes.defaultMode();
        } else {
            const std::string sceneMode = readModeFile();
            mode = sceneMode.empty() ? configuredMode : sceneMode;
        }

        const bool packageChanged = package != lastAppliedPackage;
        const bool modeChanged = mode != effectiveMode;
        if (!force && !packageChanged && !modeChanged) return;

        const bool wantOnePlus = mode == "fast" && onePlus.available() &&
                                 GamePackageFile::contains(package);
        const bool wasOnePlusActive = onePlus.active();
        const bool wantFeas = mode == "fast" && !wantOnePlus && xiaomiFeas.supports(package);
        const bool wasFeasActive = xiaomiFeas.active();

        if (!force && !modeChanged && !effectiveMode.empty() &&
            wasOnePlusActive == wantOnePlus && wasFeasActive == wantFeas) {
            {
                std::lock_guard<std::mutex> stateLock(stateMutex);
                lastAppliedPackage = package;
                lastAppliedHadExplicitRule = hasExplicitRule;
            }
            if (packageChanged) {
                lastReportedPackage = package;
                lastReportedMode = mode;
                logger.Info("情景模式 %s", mode.c_str());
            }
            return;
        }

        if (!conf.readConfig(mode)) return;

        if (wasOnePlusActive && !wantOnePlus) onePlus.setActive(false);
        xiaomiFeas.setActive(wantFeas);

        if (wantOnePlus) {
            if (!wasOnePlusActive) {
                function.CloseAllFunC();
                onePlus.setActive(true);
                applyOnePlusMode();
            } else if (packageChanged) {
                applyOnePlusMode();
            }
        } else {
            release(launchBoost);
            SchedParam();
            online();
            if (wasOnePlusActive) {
                function.AllFunC();
                logger.Info("退出风驰: 已按配置恢复附加优化");
            }
        }

        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            effectiveMode = mode;
            lastAppliedPackage = package;
            lastAppliedHadExplicitRule = hasExplicitRule;
        }

        if (packageChanged || modeChanged) {
            appModes.writeCurrentMode(mode);
            lastReportedPackage = package;
            lastReportedMode = mode;
            logger.Info("情景模式 %s", mode.c_str());
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

    void jsonTriggerTask() {
        while (true) {
            utils.InotifyMain(jsonPath, IN_CLOSE_WRITE);
            applyCurrentState(false, true);
            logger.setLogLevel(Meta::loglevel);
        }
    }

    void Init() {
        utils.Init();
        char buf[256] = { 0 };
        if (anotherInstanceRunning()) {
        const size_t runLen = utils.popenRead("pidof Littleyouran", buf, sizeof(buf) - 1);
            buf[std::min(runLen, sizeof(buf) - 1)] = '\0';
            logger.Error("CTS调度已经在运行(pid: %s), 当前进程(pid:%d)即将退出", buf, getpid());
            printf("\n!!! \n!!! CTS调度已经在运行(pid: %s), 当前进程(pid:%d)即将退出 \n!!!\n\n", buf, getpid());
            exit(-1);
        }

        if (!appModes.initialize()) exit(-1);
        conf.readConfig(appModes.defaultMode());
        logger.setLogLevel(Meta::loglevel);
        logger.Info("名称: %s", Meta::name.empty() ? "Littleyouran" : Meta::name.c_str());
        logger.Info("版本: %d", Meta::version);
        logger.Info("作者: %s", Meta::author.empty() ? "Unknown" : Meta::author.c_str());
        logger.Info("日志等级: %s", Meta::loglevel.empty() ? "INFO" : Meta::loglevel.c_str());
        const bool onePlusReady = onePlus.Init();
        const bool xiaomiReady = xiaomiFeas.Init();
        if (onePlusReady || xiaomiReady) {
            GamePackageFile::write(onePlus.packages(), xiaomiFeas.packages());
        } else {
            unlink(GamePackageFile::path);
        }
        function.AllFunC();
        release(false);
        online();
        SchedParam();
    }

private:
    std::mutex stateMutex;
    std::string lastAppliedPackage;

    std::string getCurrentPackage() {
        std::lock_guard<std::mutex> lock(stateMutex);
        return currentPackage;
    }

    std::string getEffectiveMode() {
        std::lock_guard<std::mutex> lock(stateMutex);
        return effectiveMode.empty() ? appModes.defaultMode() : effectiveMode;
    }
};