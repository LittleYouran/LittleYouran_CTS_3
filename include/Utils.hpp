#pragma once

#include <iostream>
#include <fstream>
#include <cctype>
#include <cstring>
#include <thread>
#include <string>
#include <vector>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdint>
#include <stdarg.h>
#include <stddef.h>
#include <chrono>
#include <cstdio>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <sys/system_properties.h>
#include <mutex>
#include <atomic>
#include <memory>
#include <format>
#include "LibUtils.hpp"
#include "Json/string.hpp"

// 配置编译选项 *****************
#define DEBUG_DURATION 0
#define MAX_PKG_LEN 128
#define MAX_THREAD_LEN 128
#define CPU_POLICY 8 
// *****************************

using namespace LibUtils;

using string_t = qlib::string_t;

using std::atomic;
using std::stringstream;
using std::unordered_map;
using std::lock_guard;
using std::unique_ptr;
using std::ifstream;
using std::vector;
using std::string;
using std::string_view;
using std::thread;
using std::mutex;
using std::exception;
using std::make_unique;
using std::to_string;
using std::move;


enum class LOG_LEVEL : uint32_t { 
    DEBUG = 0,
    INFO = 1,
    WARN = 2, 
    ERROR = 3, 
};

class Utils {
private:
    static constexpr const char* thermalPath = "/sys/class/thermal";
    static constexpr const char* propPath =
        "/data/adb/modules/LittleYouran/module.prop";
    static constexpr int maxBucketSize = 32;
    std::unordered_map<std::string, std::string> prop{
        {"id", "Unknown"},
        {"name", "Unknown"},
        {"version", "Unknown"},
        {"versionCode", "0"},
        {"author", "Unknown"},
        {"description", "Unknown"},
    };
public:
    // 模块身份校验：读取 module.prop 并校验 id/name/author（官方同步，大小写不敏感）
    void Init() {
        auto fp = fopen(propPath, "r");
        if (!fp) {
            printf("模块路径不存在 进程已结束\n");
            exit(-1);
        }
        char tmp[1024 * 4];
        while (fgets(tmp, sizeof(tmp), fp)) {
            if (!isalpha(static_cast<unsigned char>(tmp[0]))) continue;
            auto ptr = strchr(tmp, '=');
            if (!ptr) continue;
            *ptr = '\0';
            char* value = ptr + 1;
            value[strcspn(value, "\r\n")] = '\0';
            prop[tmp] = value;
        }
        fclose(fp);
        checkModuleProp();
    }

    void checkModuleProp() {
        auto eqI = [](const std::string& a, const char* b) {
            size_t i = 0;
            for (; i < a.size() && b[i]; ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i]))) return false;
            }
            return i == a.size() && b[i] == '\0';
        };
        if (!eqI(prop["id"], "LittleYouran") ||
            !eqI(prop["name"], "LittleYouran") ||
            !eqI(prop["author"], "LittleYouran")) {
            printf("配置文件异常 进程已退出\n");
            exit(-1);
        }
    }

    bool checkPath(const char* path) const {
        return access(path, F_OK) == 0;
    }

    // ---- 通用工具：trim / 原子写文件（重载） ----
    static std::string trim(std::string value) {
        const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        });
        const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }).base();
        return first < last ? std::string(first, last) : std::string();
    }

    // 原子写文件核心：临时文件 + fsync + rename
    static bool writeFileAtomic(const char* path, const std::string& content,
                                const struct stat* preserve,
                                const char* tmpSuffix, mode_t mode) {
        const std::string temporary = std::string(path) + tmpSuffix;
        const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                            preserve ? static_cast<mode_t>(preserve->st_mode & 07777) : mode);
        if (fd < 0) return false;

        size_t offset = 0;
        while (offset < content.size()) {
            const ssize_t written = write(fd, content.data() + offset, content.size() - offset);
            if (written <= 0) {
                close(fd);
                unlink(temporary.c_str());
                return false;
            }
            offset += static_cast<size_t>(written);
        }
        fsync(fd);
        close(fd);

        if (preserve) {
            chmod(temporary.c_str(), preserve->st_mode & 07777);
            chown(temporary.c_str(), preserve->st_uid, preserve->st_gid);
        } else {
            chmod(temporary.c_str(), mode);
        }
        if (rename(temporary.c_str(), path) != 0) {
            unlink(temporary.c_str());
            return false;
        }
        return true;
    }

    // 普通原子写（.tmp / 0664，无需保留属主）
    static bool writeFileAtomic(const char* path, const std::string& content) {
        return writeFileAtomic(path, content, nullptr, ".tmp", 0664);
    }

    // 保留原文件属主/mode（Scene SharedPreferences 需要），自定义临时后缀防与 Scene 自身写冲突
    static bool writeFileAtomic(const char* path, const std::string& content,
                                const struct stat* preserve,
                                const char* tmpSuffix = ".littleyouran.tmp") {
        return writeFileAtomic(path, content, preserve, tmpSuffix,
                               preserve ? static_cast<mode_t>(preserve->st_mode & 07777) : 0664);
    }

    int readProperty(const char* name, char* value, const size_t maxLen) noexcept {
        if (!value || maxLen == 0) return 0;
        value[0] = 0;
        char raw[PROP_VALUE_MAX] = { 0 };
        const int len = __system_property_get(name, raw);
        if (len <= 0) return 0;
        const size_t copyLen = (static_cast<size_t>(len) < maxLen - 1) ? static_cast<size_t>(len) : maxLen - 1;
        memcpy(value, raw, copyLen);
        value[copyLen] = 0;
        return static_cast<int>(copyLen);
    }

    void FileWrite(const char* filePath, const char* content) noexcept {
        int fd = open(filePath, O_WRONLY | O_NONBLOCK, 0666);
        if (fd < 0) return;
        write(fd, content, Faststrlen(content));
        close(fd);
    }

    void FileWrite(const std::string& filePath, const std::string& content) noexcept {
        int fd = open(filePath.c_str(), O_WRONLY | O_NONBLOCK, 0666);

        if (fd < 0) {
            chmod(filePath.c_str(), 0666);
            fd = open(filePath.c_str(), O_WRONLY | O_CREAT | O_NONBLOCK); 
        }

        if (fd >= 0) {
            write(fd, content.data(), content.size());
            close(fd);
        }
    }

        
    void FileWrite(const char* filePath, const string_t& content) noexcept {
        int fd = open(filePath, O_WRONLY | O_NONBLOCK, 0666);

        if (fd < 0) {
            chmod(filePath, 0666);
            fd = open(filePath, O_WRONLY | O_CREAT | O_NONBLOCK); 
        }

        if (fd >= 0) {
            write(fd, content.data(), content.size());
            close(fd);
        }
    }


    void WriteFile(const char* filePath, const char* content) noexcept {
        int fd = open(filePath, O_WRONLY | O_TRUNC | O_CREAT, 0666); 

        if (fd >= 0) {
            write(fd, content, Faststrlen(content));
            close(fd);
        }
    }

    void InotifyMain(const char* dir_name, uint32_t mask) {
        const int fd = inotify_init();
        if (fd < 0) {
            printf("Failed to initialize inotify.\n");
            exit(-1);
        }

        const int wd = inotify_add_watch(fd, dir_name, mask);
        if (wd < 0) {
            printf("Failed to add watch for directory: %s",dir_name);
            close(fd);
            exit(-1);
        }

        const int buflen = sizeof(struct inotify_event) + NAME_MAX + 1;
        char buf[buflen];
        fd_set readfds;

        while (true) {
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);

            int iRet = select(fd + 1, &readfds, nullptr, nullptr, nullptr);
            if (iRet < 0) {
                break;
            }

            int len = read(fd, buf, buflen);
            if (len < 0) {
                printf("Failed to read inotify events.\n");
                break;
            }

            const struct inotify_event *event = reinterpret_cast<const struct inotify_event *>(buf);
            if (event->mask & mask) {
                break;
            }
        }

        inotify_rm_watch(fd, wd);
        close(fd);
    }

    void sleep_ms(const int ms) {
        usleep(1000 * ms);
    }

    bool exec(const char* cmd) {
        auto fp = popen(cmd, "r");
        if (fp == nullptr) return false;
        const int status = pclose(fp);
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    int readInt(const char* path) noexcept {
        auto fd = open(path, O_RDONLY);
        if (fd < 0) return 0;

        char buff[16] = { 0 };
        auto len = read(fd, buff, sizeof(buff));
        close(fd);

        if (len <= 0) return 0;
    
        buff[15] = 0;
        return atoi(buff);
    }

    void WriteInt(const char* path, const int value) noexcept {
        auto fd = open(path, O_WRONLY);
        if (fd < 0) {
            chmod(path,0666);
            fd = open(path, O_WRONLY);
        }

        if (fd < 0) return;

        char tmp[16];
        auto len = FastSnprintf(tmp, sizeof(tmp), "%d", value);
        write(fd, tmp, len);
        close(fd);
        chmod(path, 0444);
    }

    int getPid(const char* processName) {
        DIR *dir = opendir("/proc");
        if (dir == nullptr) {
            printf("ERROR:无法打开 /proc 目录\n");
            return -1; 
        }

        struct dirent* file;
        int pid = -1;

        while ((file = readdir(dir)) != nullptr) {
            if (file->d_name[0] < '0' || file->d_name[0] > '9') continue;
            char cmdlinePath[32] = "/proc/";
            char readBuff[256];
            const size_t len = Faststrlen(file->d_name);

            memcpy(cmdlinePath + 6, file->d_name, len);
            memcpy(cmdlinePath + len + 6, "/cmdline", 9);


            if (readString(cmdlinePath, readBuff, sizeof(readBuff) - 1) <= 0) continue;
            const char *slash = strrchr(readBuff, '/');
            const char *proc_name = slash ? slash + 1 : readBuff;
            const size_t base_len = Faststrlen(proc_name);

            if (base_len == Faststrlen(processName) && !memcmp(proc_name, processName, Faststrlen(processName))) {
                pid = Fastatoi(file->d_name);
                break;
            }
        }
        closedir(dir);
        return pid;
    }

    int getTid(const char* processName, const char* comm) {
        DIR *dir = opendir("/proc");
        if (dir == nullptr) {
            printf("ERROR: 无法打开 /proc 目录\n");
            return -1;
        }
    
        struct dirent* file;
        int tid = -1;
    
        while ((file = readdir(dir)) != nullptr) {
            if (file->d_name[0] < '0' || file->d_name[0] > '9') continue;
            char cmdlinePath[256] = "/proc/";
            size_t pid_len = strlen(file->d_name);
            memcpy(cmdlinePath + 6, file->d_name, pid_len);
            memcpy(cmdlinePath + 6 + pid_len, "/cmdline", 8); 
    
            cmdlinePath[14 + pid_len] = '\0';

            char readBuff[256];
            if (readString(cmdlinePath, readBuff, sizeof(readBuff)) <= 0) continue;
    
            const char *slash = strrchr(readBuff, '/');
            const char *proc_name = slash ? slash + 1 : readBuff;
    
            if (strcmp(proc_name, processName) != 0) continue;
    
            //int pid = atoi(file->d_name);
    
            char taskPath[256] = "/proc/";
            memcpy(taskPath + 6, file->d_name, pid_len);
            memcpy(taskPath + 6 + pid_len, "/task", 5);
            taskPath[12 + pid_len] = '\0';
    
            DIR *taskDir = opendir(taskPath);
            if (taskDir == nullptr) continue;
    
            struct dirent *taskFile;
            while ((taskFile = readdir(taskDir)) != nullptr) {
                if (taskFile->d_name[0] < '0' || taskFile->d_name[0] > '9') continue;

                char commPath[256] = "/proc/";
                memcpy(commPath + 6, file->d_name, pid_len);
                memcpy(commPath + 6 + pid_len, "/task/", 7);

                size_t tid_len = strlen(taskFile->d_name);
                memcpy(commPath + 12 + pid_len, taskFile->d_name, tid_len);
                memcpy(commPath + 12 + pid_len + tid_len, "/comm", 5); 

                commPath[18 + pid_len + tid_len] = '\0';

                char Comm[256];
                if (readString(commPath, Comm, sizeof(Comm)) <= 0) continue;
    
                Comm[strcspn(Comm, "\n")] = '\0';
    
                if (!strcmp(Comm, comm)) {
                    tid = atoi(taskFile->d_name);
                    printf("进程: %s 线程: %s PID: %s TID: %d)\n", processName, comm, file->d_name, tid);
                    closedir(taskDir);
                    closedir(dir);
                    return tid;
                }
            }
            closedir(taskDir);
        }
        closedir(dir);
        return tid;
    }

    int getScreenProperty() {
        static const prop_info* pi = nullptr;

        if (pi == nullptr) {
            pi = __system_property_find("debug.tracing.screen_state");
            if (pi == nullptr) {
                return -1;
            }
        }

        char res[PROP_VALUE_MAX] = { 0 };
        __system_property_read_callback(pi,
            [](void* cookie, const char*, const char* value, unsigned) {
                if (value[0])
                    strncpy((char*)cookie, value, PROP_VALUE_MAX);
                else  ((char*)cookie)[0] = 0;
            },
            res);

        return res[0] ? res[0] - '0' : -1;
    }
    
    int openZonePath(const char* zoneName) {
        char path[256] = "/sys/class/thermal/";

        size_t zoneLen = strlen(zoneName);
        memcpy(path + 19, zoneName, zoneLen);
        memcpy(path + 19 + zoneLen, "/type", 6);

        auto fd = open(path, O_RDONLY);
        if (fd < 0) return -1;

        char buf[64];
        auto n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        
        if (n <= 0) return -1;
        
        buf[n - 1] = '\0';
        
        if (!checkSensorPath(buf)) return -1;

        memcpy(path + 19 + zoneLen, "/temp", 6);
        return open(path, O_RDONLY);
    }
    
    int readTemp(int fd) {
        char buf[32] = {0};

        lseek(fd, 0, SEEK_SET);
        auto n = read(fd, buf, sizeof(buf) - 1);
        close(fd);

        if (n <= 0) return 0;
        buf[31] = '\0';
        return atoi(buf);
    }


    int getMaxCpuTemp() {
        int maxTemp = -1;
        auto dir = opendir(thermalPath);

        if (dir == nullptr) {
            printf("无法打开文件夹:%s\n", thermalPath);
            return -1;
        }

        struct dirent* entry;
        while((entry = readdir(dir)) != nullptr) {
            if (!strncmp(entry->d_name, "thermal_zone", 12)) continue;    
            auto fd = openZonePath(entry->d_name);
            if (fd < 0) continue;
            int currentTemp = readTemp(fd);
            if (currentTemp > 0 && (maxTemp < 0 || currentTemp > maxTemp)) {
                maxTemp = currentTemp;
            }
        }

        closedir(dir);
        return (maxTemp != -1) ? maxTemp / 1000 : -1; // 舍去部分小数
    }

    void popenShell(const char* cmd, char* buf, size_t buf_size) {
        auto fp = popen(cmd, "r");
        if (!fp) return;
        while (fgets(buf, buf_size, fp) != nullptr)
        pclose(fp);
    }
    
    string getActivity() {
        char str[256];
        popenShell("dumpsys window | grep mCurrentFocus", str, sizeof(str));
        if (strstr(str, "mCurrentFocus=null")) return "null";
        const char* ptr = strstr(str, "/") + 1;
        const char* end_pos = strchr(ptr, '}');

        char activity[256];
        memcpy(activity, ptr, end_pos - ptr);
        activity[end_pos - ptr] = '\0';
        return string(activity);
    }

    string getTopApp() {
        char data[256];
        popenShell("dumpsys window | grep mCurrentFocus", data, sizeof(data));
        if (strstr(data, "mCurrentFocus=null")) return "null";
        const char* ptr = strstr(data, "u0") + 3;
        const char* end_pos = strchr(ptr, '/');

        char temp[256];
        memcpy(temp, ptr, end_pos - ptr);
        temp[end_pos - ptr] = '\0';
        return string(temp);
    }

    size_t popenRead(const char* cmd, char* buf, size_t len) {
        auto fp = popen(cmd, "r");
        if (!fp) return 0;
        auto readLen = fread(buf, 1, len, fp);
        pclose(fp);
        return readLen;
    }
    
    size_t readString(const char* path, char* buff, const size_t maxLen) {
        auto fd = open(path, O_RDONLY);
        if (fd < 0) {
            buff[0] = 0;
            return 0;
        }
        ssize_t len = read(fd, buff, maxLen);
        close(fd);
        if (len <= 0) {
            buff[0] = 0;
            return 0;
        }
        buff[len] = 0; 
        return (size_t)(len);
    }
private: 
    bool checkSensorPath(const char* str) {
        return strstr(str, "soc_max") != nullptr || strstr(str, "mtktscpu") != nullptr || strstr(str, "cpu-1-") != nullptr;
    }
};