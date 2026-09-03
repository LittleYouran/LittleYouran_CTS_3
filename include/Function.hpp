#pragma once

#include "JsonConfig.hpp"
#include "Config.hpp"
#include "Utils.hpp" 
#include "Logger.hpp"

using namespace Config;

class Function {
private:
    static constexpr const char* ufsPath = "/sys/class/block/sda";
    static constexpr const char* cpusetPath = "/dev/cpuset/";
    static constexpr const char* cpuctlPath = "/dev/cpuctl/";
    static constexpr const char* qcomGpuPath = "/sys/class/kgsl/kgsl-3d0/";
    static constexpr const char* easSchedPath = "/proc/sys/kernel/sched_energy_aware";
    static constexpr const char* mtkTouchBoostPath = "/proc/perfmgr/tchbst/user/usrtch";
    static constexpr const char* mtkPpmPolicyStatusPath = "/proc/ppm/policy_status";
    static constexpr int mtkPpmSysBoostPolicyIndex = 6;
    
    Utils utils;
    Logger logger;
public:
    void AllFunC() {
        cpusetFunction();
        disableGpuBoost();
        CfsSchedOpt();
    }
    void disableMtkTouchBoost() {
        if (access(mtkTouchBoostPath, F_OK) == 0) {
            writeNodeWithModeRestore(mtkTouchBoostPath, "enable 0");
        }

        if (access(mtkPpmPolicyStatusPath, F_OK) == 0) {
            char command[32];
            FastSnprintf(command, sizeof(command), "%d 0", mtkPpmSysBoostPolicyIndex);
            writeNodeWithModeRestore(mtkPpmPolicyStatusPath, command);
        }
    }

    bool writeNodeWithModeRestore(const char* path, const char* value) {
        struct stat nodeStat{};
        if (stat(path, &nodeStat) != 0) return false;

        const mode_t originalMode = nodeStat.st_mode & 07777;
        int fd = open(path, O_WRONLY | O_NONBLOCK);
        if (fd < 0) {
            if (chmod(path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) != 0) return false;
            fd = open(path, O_WRONLY | O_NONBLOCK);
        }
        if (fd < 0) {
            chmod(path, originalMode);
            return false;
        }

        const size_t length = Faststrlen(value);
        const bool written = write(fd, value, length) == static_cast<ssize_t>(length);
        close(fd);
        const bool restored = chmod(path, originalMode) == 0;
        return written && restored;
    }

    void CloseAllFunC() {
        char cpuOnline[64] = { 0 };
        utils.readString("/sys/devices/system/cpu/online", cpuOnline, sizeof(cpuOnline) - 1);
        if (cpuOnline[0] != '\0') {
            char* nl = strchr(cpuOnline, '\n');
            if (nl) *nl = '\0';
            const char* all = cpuOnline;   // 形如 "0-7"
            utils.FileWrite("/dev/cpuset/top-app/cpus", all);
            utils.FileWrite("/dev/cpuset/foreground/cpus", all);
            utils.FileWrite("/dev/cpuset/background/cpus", all);
            utils.FileWrite("/dev/cpuset/system-background/cpus", all);
            utils.FileWrite("/dev/cpuset/restricted/cpus", all);
            logger.Debug("Cpuset 恢复默认: %s", all);
        }

        if (checkQcom()) {
            const int levels = utils.readInt("/sys/class/kgsl/kgsl-3d0/num_pwrlevels");
            if (levels > 0) {
                const int lowestLevel = levels - 1;
                utils.WriteInt("/sys/class/kgsl/kgsl-3d0/default_pwrlevel", lowestLevel);
                utils.WriteInt("/sys/class/kgsl/kgsl-3d0/min_pwrlevel", lowestLevel);
            }
            utils.WriteInt("/sys/class/kgsl/kgsl-3d0/max_pwrlevel", 0);
        }

        utils.FileWrite("/proc/sys/kernel/sched_schedstats", "0");
        utils.FileWrite("/proc/sys/kernel/sched_latency_ns", "6000000");
        utils.FileWrite("/proc/sys/kernel/sched_migration_cost_ns", "500000");
        utils.FileWrite("/proc/sys/kernel/sched_min_granularity_ns", "1000000");
        utils.FileWrite("/proc/sys/kernel/sched_wakeup_granularity_ns", "1000000");
        utils.FileWrite("/proc/sys/kernel/sched_nr_migrate", "32");
        utils.FileWrite("/proc/sys/kernel/sched_util_clamp_min", "0");
        utils.FileWrite("/proc/sys/kernel/sched_util_clamp_max", "1024");
        if (checkEasSched()) {
            utils.FileWrite("/proc/sys/kernel/sched_energy_aware", "1");
        }
        logger.Debug("CFS 调度器参数恢复默认");

        logger.Info("已关闭附加优化");
    }
    
    void cpusetFunction() {
        if (!Cpuset::enable) return;
        utils.FileWrite("/dev/cpuset/top-app/cpus", Cpuset::top_app);
        utils.FileWrite("/dev/cpuset/foreground/cpus", Cpuset::foreground);
        utils.FileWrite("/dev/cpuset/background/cpus", Cpuset::background);
        utils.FileWrite("/dev/cpuset/system-background/cpus", Cpuset::system_background);
        utils.FileWrite("/dev/cpuset/restricted/cpus", Cpuset::restricted);

        logger.Debug("top_app: %s", Cpuset::top_app.c_str());
        logger.Debug("foreground: %s", Cpuset::foreground.c_str());
        logger.Debug("background: %s", Cpuset::background.c_str());
        logger.Debug("system_background: %s", Cpuset::system_background.c_str());
        logger.Debug("restricted: %s", Cpuset::restricted.c_str());

        logger.Info("CpuSet调整完毕");
    }

    
    void disableGpuBoost() {
        if (!checkQcom() || !DisableGpuBoost::enable) return;
        const int buff = utils.readInt("/sys/class/kgsl/kgsl-3d0/num_pwrlevels");
        const int minPwrlvl = buff - 1;
        utils.WriteInt("/sys/class/kgsl/kgsl-3d0/default_pwrlevel", minPwrlvl);
        utils.WriteInt("/sys/class/kgsl/kgsl-3d0/min_pwrlevel", minPwrlvl);
    
        utils.FileWrite("/sys/class/kgsl/kgsl-3d0/max_pwrlevel", "0");
        utils.FileWrite("/sys/class/kgsl/kgsl-3d0/thermal_pwrlevel", "0");   
        utils.FileWrite("/sys/class/kgsl/kgsl-3d0/throttling", "0");
        utils.FileWrite("/sys/class/kgsl/kgsl-3d0/force_bus_on", "0");
        utils.FileWrite("/sys/class/kgsl/kgsl-3d0/force_clk_on", "0");
        utils.FileWrite("/sys/class/kgsl/kgsl-3d0/force_no_nap", "0");
        utils.FileWrite("/sys/class/kgsl/kgsl-3d0/force_rail_on", "0");     
        utils.FileWrite("/sys/class/kgsl/kgsl-3d0/bus_split", "1"); 

        logger.Debug("default_pwrlevel调整为: %d", minPwrlvl);
        logger.Debug("min_pwrlevel调整为: %d", minPwrlvl);
        logger.Debug("max_pwrlevel调整为: 0");
        logger.Debug("thermal_pwrlevel调整为: 0");
        logger.Debug("throttling调整为: 0");
        logger.Debug("force_bus_on调整为: 0");
        logger.Debug("force_clk_on调整为: 0");
        logger.Debug("force_no_nap调整为: 0");
        logger.Debug("force_rail_on调整为: 0");
        logger.Debug("bus_split调整为: 1");

        logger.Info("高通GPU已优化完毕");
    }

    void CfsSchedOpt() {
        if (!Scheduler::enable) return;
        utils.FileWrite("/proc/sys/kernel/sched_schedstats", Scheduler::Sched_schedstats ? "1" : "0");
        utils.FileWrite("/proc/sys/kernel/sched_latency_ns", Scheduler::Sched_latency_ns);
        utils.FileWrite("/proc/sys/kernel/sched_migration_cost_ns", Scheduler::Sched_migration_cost_ns);
        utils.FileWrite("/proc/sys/kernel/sched_min_granularity_ns", Scheduler::Sched_min_granularity_ns);
        utils.FileWrite("/proc/sys/kernel/sched_wakeup_granularity_ns", Scheduler::Sched_wakeup_granularity_ns);
        utils.FileWrite("/proc/sys/kernel/sched_nr_migrate", Scheduler::Sched_nr_migrate);
        utils.FileWrite("/proc/sys/kernel/sched_util_clamp_min", Scheduler::Sched_util_clamp_min);
        utils.FileWrite("/proc/sys/kernel/sched_util_clamp_max", Scheduler::Sched_util_clamp_max);
        
        if (checkEasSched()) {
            utils.FileWrite("/proc/sys/kernel/sched_energy_aware", Scheduler::Sched_energy_aware ? "1" : "0");
            logger.Info(Scheduler::Sched_energy_aware ? "已开启EAS调度器" : "已关闭EAS调度器");
        } else
            logger.Error("您的设备并不支持EAS调度器");

        logger.Debug("Sched_energy_aware调整为: %s", Scheduler::Sched_energy_aware ? "开启" : "关闭");
        logger.Debug("Sched_schedstats调整为: %s", Scheduler::Sched_schedstats ? "开启" : "关闭");
        logger.Debug("Sched_latency_ns调整为: %s", Scheduler::Sched_latency_ns.c_str());
        logger.Debug("Sched_migration_cost_ns调整为: %s", Scheduler::Sched_migration_cost_ns.c_str());
        logger.Debug("Sched_wakeup_granularity_ns调整为: %s", Scheduler::Sched_wakeup_granularity_ns.c_str());
        logger.Debug("Sched_nr_migrate调整为: %s", Scheduler::Sched_nr_migrate.c_str());
        logger.Debug("Sched_util_clamp_min调整为: %s", Scheduler::Sched_util_clamp_min.c_str());
        logger.Debug("Sched_util_clamp_max调整为: %s", Scheduler::Sched_util_clamp_max.c_str());

        logger.Info("CFS调度器已优化完毕");
    }

    bool checkQcom() const {
        return (!access(qcomGpuPath, F_OK));
    }
private:
    bool checkEasSched() const {
        return (!access(easSchedPath, F_OK));
    }
};