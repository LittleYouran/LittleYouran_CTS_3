#pragma once

#include "LibUtils.hpp"
#include "Utils.hpp"
#include "Logger.hpp"
#include <cstring>
#include <string>

class Detector {
private:
    Utils utils;
    Logger logger;

    static constexpr const char* topAppProcs = "/dev/cpuset/top-app/cgroup.procs";
    static constexpr const char* fgProcs = "/dev/cpuset/foreground/cgroup.procs";

public:
    Detector() = default;

    /* 获取前台包名 */
    std::string getTopApp() {
        char data[256] = {0};
        utils.popenShell("dumpsys window | grep mCurrentFocus", data, sizeof(data));
        if (strstr(data, "mCurrentFocus=null")) return "";

        const char* ptr = strstr(data, "u0");
        if (!ptr) return "";
        ptr += 3;

        const char* end = strchr(ptr, '/');
        if (!end) return "";

        return std::string(ptr, end - ptr);
    }

    /* 判断在不在前台 cpuset 中（top-app + foreground） */
    bool isInForegroundCpuset(const char* packageName) {
        static const char* paths[] = { topAppProcs, fgProcs };

        for (int k = 0; k < 2; k++) {
            char buf[1024] = {0};
            int fd = open(paths[k], O_RDONLY);
            if (fd < 0) continue;

            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0) continue;

            buf[n] = 0;
            char* pidStr = strtok(buf, " \n\t");
            while (pidStr) {
                int pid = atoi(pidStr);
                if (pid <= 0) { pidStr = strtok(nullptr, " \n\t"); continue; }

                char cmdPath[64];
                char cmd[256] = {0};
                FastSnprintf(cmdPath, sizeof(cmdPath), "/proc/%d/cmdline", pid);

                int cfd = open(cmdPath, O_RDONLY);
                if (cfd < 0) { pidStr = strtok(nullptr, " \n\t"); continue; }

                ssize_t cn = read(cfd, cmd, sizeof(cmd) - 1);
                close(cfd);
                if (cn <= 0) { pidStr = strtok(nullptr, " \n\t"); continue; }

                cmd[cn] = 0;
                if (strstr(cmd, packageName)) { return true; }
                pidStr = strtok(nullptr, " \n\t");
            }
        }
        return false;
    }
};
