#pragma once

#include "Utils.hpp"
#include "Logger.hpp"
#include "Function.hpp"
#include <poll.h>
#include <errno.h>

// 风驰: 游戏识别与极速模式切换
// 来源  /data/adb/modules/RemoteConfigOverride/json/ 文件名
class OnePlus {
private:
    static constexpr const char* topAppProcs = "/dev/cpuset/top-app/cgroup.procs";
    static constexpr const char* modePath = "/sdcard/Android/CTS/mode.txt";
    static constexpr const char* availGovPath =
        "/sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors";
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
    bool mkdirDone = false;
    bool horaeActive = false;

    // 从 MD 读取: 数据库不可用时的包名兜底来源
    void loadPackagesFromMd() {
        gamePackages.clear();

        char content[8192] = { 0 };
        const size_t len = utils.readString(mdPath, content, sizeof(content) - 1);
        if (len == 0) return;
        content[len] = '\0';

        char* p = content;
        char* end = content + len;
        while (p < end) {
            char* nl = strchr(p, '\n');
            const size_t lineLen = nl ? (size_t)(nl - p) : (size_t)(end - p);
            if (lineLen > 0) {
                size_t trim = lineLen;
                while (trim > 0 && (p[trim - 1] == '\r' || p[trim - 1] == ' ' || p[trim - 1] == '\t')) trim--;
                if (trim > 0) gamePackages.emplace_back(p, trim);
            }
            if (!nl) break;
            p = nl + 1;
        }
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

    // 检测顺序: hmbird → scx (仅用于风驰频率不限制)
    // 均未检测到时返回 nullptr: 进入风驰按配置文件走
    const char* detectGovernor() {
        char buf[512] = { 0 };
        const int fd = open(availGovPath, O_RDONLY);
        if (fd < 0) return nullptr;

        const ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return nullptr;
        buf[n] = 0;

        static const char* candidates[] = { "hmbird", "scx" };
        for (auto g : candidates) {
            if (strstr(buf, g)) {
                logger.Debug("风驰: 检测到调速器 %s", g);
                return g;
            }
        }
        return nullptr;
    }

    // 检测 hmbird/scx 调速器是否可用 (无则风驰不可用)
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

    // 小窗检测: 仅当游戏为前台焦点且窗口全屏时判定活跃
    // 2.1 游戏霸占全屏 → 活跃
    // 2.2 游戏霸占全屏 + 其他应用小窗 → 仍活跃 (焦点仍在游戏)
    // 2.3 其他应用全屏 + 游戏小窗 → 不活跃
    // 2.4 游戏在后台 → 不活跃
    bool isGameActive() {
        if (gamePackages.empty()) return false;

        // 1) 当前焦点窗口是否属于游戏
        char focus[512] = { 0 };
        utils.popenRead("dumpsys window | grep mCurrentFocus", focus, sizeof(focus) - 1);
        if (!strstr(focus, "Window{")) return false;   // 无焦点窗口
        const char* slash = strchr(focus, '/');
        if (!slash) return false;
        const char* pkgStart = slash;
        while (pkgStart > focus && pkgStart[-1] != ' ' && pkgStart[-1] != '{') pkgStart--;

        bool isGameFocus = false;
        for (const auto& pkg : gamePackages) {
            if ((size_t)(slash - pkgStart) == pkg.size() &&
                strncmp(pkgStart, pkg.c_str(), pkg.size()) == 0) {
                isGameFocus = true;
                break;
            }
        }
        if (!isGameFocus) return false;   // 2.3 / 2.4: 焦点不是游戏

        // 2) 焦点是游戏 → 判定窗口是否全屏 (2.1 / 2.2)
        // 屏幕尺寸
        int sw = 0, sh = 0;
        char out[128] = { 0 };
        utils.popenRead("wm size", out, sizeof(out) - 1);
        const char* sz = strstr(out, "size:");
        if (sz) {
            sz += 5;
            while (*sz == ' ' || *sz == '\t') sz++;
            sw = atoi(sz);
            const char* x = strchr(sz, 'x');
            if (x) sh = atoi(x + 1);
        }
        if (sw <= 0 || sh <= 0) return true;   // 拿不到尺寸时保守按活跃

        // 焦点窗口 mFrame (grep 过滤后解析)
        char win[16384] = { 0 };
        utils.popenRead("dumpsys window windows | grep -E 'Window #|mCurrentFocus=true|mFrame='",
                        win, sizeof(win) - 1);
        const char* cur = strstr(win, "mCurrentFocus=true");
        if (!cur) return true;   // 取不到焦点窗口时保守按活跃

        // 向上找包含焦点窗口的窗口块头
        const char* blockHead = win;
        const char* search = win;
        while ((search = strstr(search, "Window #")) != nullptr && search < cur) {
            blockHead = search;
            search++;
        }
        // 块内找 mFrame=[l,t][r,b]
        const char* frame = strstr(blockHead, "mFrame=");
        if (!frame) return true;
        const char* rb = strchr(frame, ']');
        if (!rb) return true;
        const char* lb = rb + 1;                 // [r,b]
        if (*lb != '[') return true;
        const int fw = atoi(lb + 1);
        const char* comma = strchr(lb, ',');
        const int fh = comma ? atoi(comma + 1) : 0;
        if (fw <= 0 || fh <= 0) return true;

        // 全屏容差 10%
        return (fw >= sw * 9 / 10 && fh >= sh * 9 / 10);
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

    // 切换逻辑 (最简约): 进入游戏切 fast, 退出恢复原模式
    void enterGame() {
        if (!defaultMode.empty()) return;   // 已在游戏模式

        {
            std::lock_guard<std::mutex> lock(Config::applyMutex);
            GameMode::active = true;
            defaultMode = readMode();
            if (defaultMode.empty()) {
                GameMode::active = false;
                logger.Warn("风驰: 读取当前模式失败");
                defaultMode.clear();
                return;
            }

            writeMode("fast");
            logger.Info("风驰: 切换为极速模式 (原模式: %s)", defaultMode.c_str());
        }

        // 进入风驰: 关闭所有附加优化, 游戏满血运行
        Function function;
        function.CloseAllFunC();
        logger.Info("风驰: 已关闭附加优化");

        setHorae(true);
    }

    void exitGame() {
        if (defaultMode.empty()) return;    // 非游戏模式

        const std::string restoreMode = defaultMode;
        defaultMode.clear();

        {
            std::lock_guard<std::mutex> lock(Config::applyMutex);
            GameMode::active = false;
            writeMode(restoreMode.c_str());
            logger.Info("风驰: 恢复模式 %s", restoreMode.c_str());
        }

        // 退出风驰: 重新应用附加优化
        Function function;
        function.AllFunC();
        logger.Info("退出风驰: 已恢复额外优化");

        setHorae(false);
    }

    void monitorTask() {
        if (gamePackages.empty()) return;

        sleep(2);

        const int fd = inotify_init();
        if (fd < 0) {
            logger.Error("风驰: 无法初始化 inotify");
            return;
        }

        const int wdCpuset = inotify_add_watch(fd, topAppProcs, IN_MODIFY);
        const int wdMd = inotify_add_watch(fd, mdPath, IN_MODIFY | IN_CLOSE_WRITE);
        if (wdCpuset < 0 || wdMd < 0) {
            logger.Error("风驰: inotify 监听失败");
            close(fd);
            return;
        }

        logger.Info("风驰: 已开始监听前台应用切换");

        char buf[8192];
        while (true) {
            // 事件驱动: 前台切换或 MD 变化时触发检测
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
                if (gamePackages.empty()) loadPackagesFromMd();
                if (gamePackages.empty()) continue;
            }

            utils.sleep_ms(300);   // 防抖

            if (isGameActive()) {
                enterGame();
            } else {
                exitGame();
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

        loadPackagesFromMd();   // 先读 MD 兜底
        loadPackages();         // 数据库刷新并写 MD
        logger.Info("风驰: 游戏包名就绪 (%u 个)", static_cast<unsigned>(gamePackages.size()));
    }

    void Start() {
        std::thread(&OnePlus::monitorTask, this).detach();
    }

    bool isGameModeActive() {
        return isGameActive();
    }

    const char* getGovernor() {
        return detectGovernor();
    }
};
