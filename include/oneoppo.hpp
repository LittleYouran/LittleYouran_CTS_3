#pragma once

#include "Utils.hpp"
#include "Logger.hpp"
#include "Config.hpp"
#include "Function.hpp"

// 来源  /data/adb/modules/RemoteConfigOverride/json/ 文件名
class OneOppo {
private:
    static constexpr const char* topAppProcs = "/dev/cpuset/top-app/cgroup.procs";
    static constexpr const char* fgProcs = "/dev/cpuset/foreground/cgroup.procs";
    static constexpr const char* modePath = "/sdcard/Android/CTS/mode.txt";
    static constexpr const char* availGovPath =
        "/sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors";
    static constexpr const char* qcomGpuPath = "/sys/class/kgsl/kgsl-3d0/";
    static constexpr const char* sqlitePath =
        "/data/adb/modules/LittleYouran/tool/sqlite3";
    static constexpr const char* dbPath1 =
        "/data/data/com.oplus.cosa/databases/db_game_database";
    static constexpr const char* dbPath2 =
        "/data/user_de/0/com.oplus.cosa/databases/db_game_database";
    static constexpr const char* mdDir =
        "/storage/emulated/0/Android/CTS";
    static constexpr const char* mdPath =
        "/storage/emulated/0/Android/CTS/game_packages.md";

    Utils utils;
    Logger logger;

        char temp[256];
    std::vector<std::string> gamePackages;
    std::string lastMdContent;
    std::string defaultMode;
    bool horaeActive = false;
    bool mkdirDone = false; 
    bool checkQcom() const {
        return (!access(qcomGpuPath, F_OK));
    }

    bool checkOfficialGovAvailable() {
        char buf[512] = { 0 };
        const int fd = open(availGovPath, O_RDONLY);
        if (fd < 0) return false;

        const ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return false;
        buf[n] = 0;

        return (strstr(buf, "hmbird") != nullptr || strstr(buf, "scx") != nullptr);
    }

    // 优先级: hmbird > scx > walt > sugov_ext
    const char* detectGovernor() {
        char buf[512] = { 0 };
        const int fd = open(availGovPath, O_RDONLY);
        if (fd < 0) return checkQcom() ? "walt" : "sugov_ext";

        const ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return checkQcom() ? "walt" : "sugov_ext";
        buf[n] = 0;

        static const char* candidates[] = { "hmbird", "scx", "walt", "sugov_ext", "schedutil" };
        for (auto g : candidates) {
            if (strstr(buf, g)) {
                logger.Debug("风驰: 检测到调速器 %s", g);
                return g;
            }
        }
        return checkQcom() ? "walt" : "sugov_ext";
    }


    // 包名自动获取: sqlite3 查数据库(只读) → 过滤系统组件 → 写 MD
    void loadPackages() {
        const char* db = nullptr;
        if (access(dbPath1, F_OK) == 0) db = dbPath1;
        else if (access(dbPath2, F_OK) == 0) db = dbPath2;

        if (db == nullptr) {
            logger.Warn("风驰: 没有检测到包名 缺少包名或对应路径");
            return;
        }

        gamePackages.clear();

        // 执行: sqlite3 <db> "SELECT package_name FROM PackageConfigBean;"
        FastSnprintf(temp, sizeof(temp),
                     "%s %s \"SELECT package_name FROM PackageConfigBean;\"",
                     sqlitePath, db);

        char out[4096] = { 0 };
        const size_t len = utils.popenRead(temp, out, sizeof(out) - 1);
        if (len == 0) {
        } else {
            out[len] = '\0';

            // 逐行解析 过滤系统组件 (com.oplus.* / oplus.cosa.*)
            char* p = out;
            char* end = out + len;
            while (p < end) {
            char* nl = strchr(p, '\n');
            const size_t lineLen = nl ? (size_t)(nl - p) : (size_t)(end - p);
            if (lineLen > 0) {
                size_t trim = lineLen;
                while (trim > 0 && (p[trim - 1] == '\r' || p[trim - 1] == ' ')) trim--;
                if (trim > 0 &&
                    !startWith(p, "com.oplus.") &&
                    !startWith(p, "oplus.cosa.")) {
                    gamePackages.emplace_back(p, trim);
                }
            }
                if (!nl) break;
                p = nl + 1;
            }
        }

        if (!mkdirDone) {
            utils.exec("mkdir -p /storage/emulated/0/Android/CTS");
            mkdirDone = true;
        }
        std::string mdContent;
        for (const auto& pkg : gamePackages) {
            mdContent += pkg;
            mdContent += '\n';
        }
        if (mdContent != lastMdContent) {
            utils.WriteFile(mdPath, mdContent.c_str());
            lastMdContent = mdContent;
        }

        if (gamePackages.empty()) {
            logger.Warn("风驰: 没有检测到包名 缺少包名或对应路径");
        } else {
            logger.Info("风驰: 检测到包名 (%u 个)", static_cast<unsigned>(gamePackages.size()));
        }
    }

    bool isInCpuset(const char* cpusetPath, const char* pkg) {
        char buf[1024] = { 0 };
        const int fd = open(cpusetPath, O_RDONLY);
        if (fd < 0) return false;

        const ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return false;
        buf[n] = 0;

        char* pidStr = strtok(buf, " \n\t");
        while (pidStr) {
            const int pid = atoi(pidStr);
            if (pid > 0) {
                char cmdPath[64];
                char cmd[256] = { 0 };
                FastSnprintf(cmdPath, sizeof(cmdPath), "/proc/%d/cmdline", pid);

                const int cfd = open(cmdPath, O_RDONLY);
                if (cfd >= 0) {
                    const ssize_t cn = read(cfd, cmd, sizeof(cmd) - 1);
                    close(cfd);
                    if (cn > 0) {
                        cmd[cn] = 0;
                        if (strstr(cmd, pkg)) return true;
                    }
                }
            }
            pidStr = strtok(nullptr, " \n\t");
        }
        return false;
    }

    bool isGameActive() {
        if (gamePackages.empty()) return false;

        for (const auto& pkg : gamePackages) {
            if (isInCpuset(topAppProcs, pkg.c_str())) return true;
            if (isInCpuset(fgProcs, pkg.c_str())) return true;
        }
        return false;
    }

    bool confirmInactive() {
        for (int retry = 0; retry < 2; retry++) {
            utils.sleep_ms(300);
            if (isGameActive()) return false;  // 不退出
        }
        return true;
    }


    std::string readMode() {
        char buf[64] = { 0 };
        utils.readString(modePath, buf, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char* start = buf;
        while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
        char* end = start + Faststrlen(start);
        while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
        return std::string(start, end - start);
    }

    void writeMode(const char* mode) {
        utils.WriteFile(modePath, mode);
    }


    void setHorae(bool enable) {
        if (enable == horaeActive) return;

        system(enable ? "setprop persist.sys.horae.enable 1"
                      : "setprop persist.sys.horae.enable 0");

        char result[32] = { 0 };
        utils.popenRead("getprop persist.sys.horae.enable", result, sizeof(result));

        if (enable) {
            if (result[0] == '1') logger.Info("horae 已开启");
            else logger.Warn("horae 开启失败: %s", result);
        } else {
            if (result[0] == '0' || result[0] == 0) logger.Info("horae 已关闭");
            else logger.Warn("horae 关闭失败: %s", result);
        }
        horaeActive = enable;
    }

    void enterGame() {
        GameMode::active = true;
        defaultMode = readMode();
        if (defaultMode.empty()) {
            GameMode::active = false;
            logger.Warn("风驰: 读取当前模式失败");
            return;
        }

        writeMode("fast");
        logger.Info("风驰: 切换为极速模式 风驰已开启 (原模式: %s)", defaultMode.c_str());

        setHorae(true);
    }

    void exitGame() {
        if (readMode() == "fast") {
            writeMode(defaultMode.c_str());
            logger.Info("风驰: 游戏已退出 恢复模式 %s", defaultMode.c_str());
        } else {
            logger.Info("风驰: 游戏已退出 当前模式 %s 非 fast 跳过恢复", readMode().c_str());
        }
        defaultMode.clear();

        GameMode::active = false;
        Function function;
        function.AllFunC();
        logger.Info("退出风驰: 已恢复额外优化");

        setHorae(false);
    }


    void monitorTask() {
        if (gamePackages.empty()) return;

        sleep(2);

        if (isGameActive() && defaultMode.empty()) {
            enterGame();
        }

        const int fd = inotify_init();
        if (fd < 0) {
            logger.Error("风驰: 无法初始化 inotify");
            exit(-1);
        }

        const int wdCpuset = inotify_add_watch(fd, topAppProcs, IN_MODIFY);
        const int wdMd = inotify_add_watch(fd, mdPath, IN_MODIFY | IN_CLOSE_WRITE);
        if (wdCpuset < 0 || wdMd < 0) {
            logger.Error("风驰: inotify 监听失败");
            exit(-1);
        }

        logger.Info("风驰: 已开始监听前台应用切换");

        char buf[8192];
        while (true) {
            const ssize_t len = read(fd, buf, sizeof(buf));
            if (len <= 0) continue;

            bool mdChanged = false;
            const char* p = buf;
            const char* pEnd = buf + len;
            while (p + (ssize_t)sizeof(struct inotify_event) <= pEnd) {
                const struct inotify_event* ev = (const struct inotify_event*)p;
                if (ev->wd == wdMd) mdChanged = true;
                p += sizeof(struct inotify_event) + ev->len;
            }

            if (mdChanged) {
                logger.Info("风驰: 检测到 MD 文件变化 重新获取包名");
                loadPackages();
                if (gamePackages.empty()) {
                    if (!defaultMode.empty()) exitGame();
                    continue;
                }
            }

            utils.sleep_ms(500); 

            if (isGameActive()) {
                if (defaultMode.empty()) {
                    enterGame();
                } else if (readMode() != "fast") {
                    writeMode("fast");
                    logger.Info("风驰: 模式被外部修改 重新切换极速模式");
                }
            } else {
                if (!defaultMode.empty() && confirmInactive()) exitGame();
            }
        }

        inotify_rm_watch(fd, wdCpuset);
        inotify_rm_watch(fd, wdMd);
        close(fd);
    }

public:
    void Init() {
        if (!checkOfficialGovAvailable()) {
            logger.Warn("风驰不可用 (未检测到 hmbird/scx 调速器)");
            return;
        }
        logger.Info("HMBird/SCX detected. 风驰可用");

        loadPackages();
    }

    void Start() {
        std::thread(&OneOppo::monitorTask, this).detach();
    }

    bool isGameModeActive() {
        return isGameActive();
    }

    const char* getGovernor() {
        return detectGovernor();
    }
};