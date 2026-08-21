#pragma once

#include "JsonConfig.hpp"
#include "Config.hpp"
#include "Utils.hpp" 
#include "Logger.hpp"

using namespace Config;

class Function {
private:
    static constexpr const char* qcomFeas = "/sys/module/perfmgr/parameters/perfmgr_enable";
    static constexpr const char* mtkFeas = "/sys/module/mtk_fpsgo/parameters/perfmgr_enable";
    static constexpr const char* ufsPath = "/sys/class/block/sda";
    static constexpr const char* cpusetPath = "/dev/cpuset/";
    static constexpr const char* cpuctlPath = "/dev/cpuctl/";
    static constexpr const char* qcomGpuPath = "/sys/class/kgsl/kgsl-3d0/";
    static constexpr const char* easSchedPath = "/proc/sys/kernel/sched_energy_aware";
    
    Utils utils;
    Logger logger;
public:
    void AllFunC() {
        cpusetFunction();
        disableGpuBoost();
        CfsSchedOpt();
    }

    // 关闭附加优化: 进入风驰(游戏)时调用, 恢复系统默认; 退出风驰时由 AllFunC 重新应用
    void CloseAllFunC() {
        // Cpuset 恢复默认(全部在线核心放开)
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

        // GPU 恢复默认(性能层级0, 不再限制)
        if (checkQcom()) {
            utils.WriteInt("/sys/class/kgsl/kgsl-3d0/default_pwrlevel", 0);
            utils.WriteInt("/sys/class/kgsl/kgsl-3d0/min_pwrlevel", 0);
            utils.WriteInt("/sys/class/kgsl/kgsl-3d0/max_pwrlevel", 0);
            logger.Debug("GPU 恢复默认性能层级");
        }

        // CFS 调度器参数恢复内核默认
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

        logger.Info("风驰: 已关闭附加优化");
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

    bool FeasFunc(bool Enable) {
        // 后续可直接获取前台 检测是否是游戏 来确定是否需要开启"Feas"
        if (checkQcomFeas()) {
            utils.FileWrite(qcomFeas, Enable ? "1" : "0");
            logger.Debug("QCOM Feas 已%s", Enable ? "开启" : "关闭");
            return true;
        } 

        if (checkMtkFeas()) {
            utils.FileWrite(mtkFeas, Enable ? "1" : "0");
            logger.Debug("MTK Feas 已%s", Enable ? "开启" : "关闭");
            return true;
        }
        return false;
    }

    bool checkQcom() const {
        return (!access(qcomGpuPath, F_OK));
    }
private:
    bool checkQcomFeas() const {
        return (!access(qcomFeas, F_OK));
    }
    
    bool checkMtkFeas() const {
        return (!access(mtkFeas, F_OK));
    }
    
    bool checkEasSched() const {
        return (!access(easSchedPath, F_OK));
    }
};
