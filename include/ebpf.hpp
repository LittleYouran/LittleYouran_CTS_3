#pragma once

#include "Utils.hpp"
#include "Logger.hpp"
#include <linux/bpf.h>
#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <poll.h>

// eBPF 事件源: tracepoint cgroup/cgroup_attach_task → perf buffer 回传 pid
// 事件驱动: poll 阻塞等待, 空闲 0 CPU; 初始化失败返回 false, 由调用方回退 inotify
class Ebpf {
private:
    static constexpr int MAX_CPU = 8;
    static constexpr int PERF_PAGES = 1 + 32;   // 128KB/cpu, 防事件风暴丢事件(丢事件会导致状态卡死)

    Utils utils;
    Logger logger;

    int progFd = -1;
    int outFd = -1;
    int tpFd = -1;
    int mdFd = -1;
    int mdWd = -1;
    int perfFds[MAX_CPU];
    void* bases[MAX_CPU] = { nullptr };
    bool ready = false;

    static long bpfSys(int cmd, void* attr, size_t size) {
        return syscall(__NR_bpf, cmd, attr, size);
    }

    // 关闭所有资源 (失败清理)
    void cleanup() {
        if (mdFd >= 0) close(mdFd);
        if (tpFd >= 0) close(tpFd);
        if (progFd >= 0) close(progFd);
        for (int i = 0; i < MAX_CPU; i++) {
            if (bases[i]) { munmap(bases[i], 4096 * PERF_PAGES); bases[i] = nullptr; }
            if (perfFds[i] > 0) close(perfFds[i]);
            perfFds[i] = -1;
        }
        if (outFd >= 0) close(outFd);
        tpFd = progFd = outFd = mdFd = -1;
        ready = false;
    }

public:
    Ebpf() {
        for (int i = 0; i < MAX_CPU; i++) perfFds[i] = -1;
    }
    ~Ebpf() { cleanup(); }

    bool ready_() const { return ready; }

    // 初始化: map + prog + 每 cpu perf buffer + attach
    bool Init() {
        union bpf_attr ma;
        memset(&ma, 0, sizeof(ma));
        ma.map_type = BPF_MAP_TYPE_PERF_EVENT_ARRAY;
        ma.key_size = 4;
        ma.value_size = 4;
        ma.max_entries = MAX_CPU;
        outFd = (int)bpfSys(BPF_MAP_CREATE, &ma, sizeof(ma));
        if (outFd < 0) { logger.Warn("风驰: eBPF map 创建失败"); return false; }

        // eBPF 程序: 输出 ctx->pid (offset 24) 到当前 cpu 的 perf buffer
        struct bpf_insn insns[] = {
            { .code = BPF_ALU64 | BPF_MOV | BPF_X, .dst_reg = BPF_REG_6, .src_reg = BPF_REG_1 },
            { .code = BPF_LDX | BPF_MEM | BPF_W, .dst_reg = BPF_REG_1, .src_reg = BPF_REG_6, .off = 24 },
            { .code = BPF_STX | BPF_MEM | BPF_W, .dst_reg = BPF_REG_10, .src_reg = BPF_REG_1, .off = -4 },
            { .code = BPF_JMP | BPF_CALL, .imm = 8 },   // r0 = smp_processor_id
            { .code = BPF_ALU64 | BPF_MOV | BPF_X, .dst_reg = BPF_REG_7, .src_reg = BPF_REG_0 },
            { .code = BPF_ALU64 | BPF_MOV | BPF_X, .dst_reg = BPF_REG_1, .src_reg = BPF_REG_6 },
            { .code = BPF_LD | BPF_DW | BPF_IMM, .dst_reg = BPF_REG_2, .src_reg = BPF_PSEUDO_MAP_FD, .imm = outFd },
            { .code = 0, .imm = 0 },
            { .code = BPF_ALU64 | BPF_MOV | BPF_X, .dst_reg = BPF_REG_3, .src_reg = BPF_REG_7 },
            { .code = BPF_ALU64 | BPF_MOV | BPF_X, .dst_reg = BPF_REG_4, .src_reg = BPF_REG_10 },
            { .code = BPF_ALU64 | BPF_ADD | BPF_K, .dst_reg = BPF_REG_4, .imm = -4 },
            { .code = BPF_ALU64 | BPF_MOV | BPF_K, .dst_reg = BPF_REG_5, .imm = 4 },
            { .code = BPF_JMP | BPF_CALL, .imm = 25 },  // perf_event_output
            { .code = BPF_ALU64 | BPF_MOV | BPF_K, .dst_reg = BPF_REG_0, .imm = 0 },
            { .code = BPF_JMP | BPF_EXIT },
        };
        char logBuf[4096] = { 0 };
        union bpf_attr pa;
        memset(&pa, 0, sizeof(pa));
        pa.prog_type = BPF_PROG_TYPE_TRACEPOINT;
        pa.insn_cnt = sizeof(insns) / sizeof(insns[0]);
        pa.insns = (unsigned long)insns;
        pa.license = (unsigned long)"GPL";
        pa.log_buf = (unsigned long)logBuf;
        pa.log_size = sizeof(logBuf);
        pa.log_level = 1;
        progFd = (int)bpfSys(BPF_PROG_LOAD, &pa, sizeof(pa));
        if (progFd < 0) { logger.Warn("风驰: eBPF prog 加载失败"); cleanup(); return false; }

        // 每 cpu 一个 perf buffer (不 enable, enable 会重设 oncpu 破坏 cpu 匹配)
        struct perf_event_attr pe;
        memset(&pe, 0, sizeof(pe));
        pe.type = PERF_TYPE_SOFTWARE;
        pe.size = sizeof(pe);
        pe.config = PERF_COUNT_SW_BPF_OUTPUT;
        pe.sample_type = PERF_SAMPLE_RAW;
        pe.sample_period = 1;
        pe.wakeup_events = 1;
        for (int c = 0; c < MAX_CPU; c++) {
            perfFds[c] = (int)syscall(__NR_perf_event_open, &pe, -1, c, -1, PERF_FLAG_FD_CLOEXEC);
            if (perfFds[c] < 0) { logger.Warn("风驰: eBPF perf_event_open cpu%d 失败", c); cleanup(); return false; }
            bases[c] = mmap(NULL, 4096 * PERF_PAGES, PROT_READ | PROT_WRITE, MAP_SHARED, perfFds[c], 0);
            if (bases[c] == MAP_FAILED) { bases[c] = nullptr; logger.Warn("风驰: eBPF mmap cpu%d 失败", c); cleanup(); return false; }
            int key = c;
            union bpf_attr ua;
            memset(&ua, 0, sizeof(ua));
            ua.map_fd = outFd;
            ua.key = (unsigned long)&key;
            ua.value = (unsigned long)&perfFds[c];
            ua.flags = BPF_ANY;
            if (bpfSys(BPF_MAP_UPDATE_ELEM, &ua, sizeof(ua)) < 0) {
                logger.Warn("风驰: eBPF map update cpu%d 失败", c);
                cleanup();
                return false;
            }
        }

        // attach tracepoint
        int idFd = open("/sys/kernel/tracing/events/cgroup/cgroup_attach_task/id", O_RDONLY);
        if (idFd < 0) { logger.Warn("风驰: eBPF tracepoint id 不可用"); cleanup(); return false; }
        char idBuf[32] = { 0 };
        read(idFd, idBuf, sizeof(idBuf) - 1);
        close(idFd);
        struct perf_event_attr tpAttr;
        memset(&tpAttr, 0, sizeof(tpAttr));
        tpAttr.type = PERF_TYPE_TRACEPOINT;
        tpAttr.size = sizeof(tpAttr);
        tpAttr.config = atoi(idBuf);
        tpAttr.sample_period = 1;
        tpAttr.wakeup_events = 1;
        tpFd = (int)syscall(__NR_perf_event_open, &tpAttr, -1, 0, -1, PERF_FLAG_FD_CLOEXEC);
        if (tpFd < 0 || ioctl(tpFd, PERF_EVENT_IOC_SET_BPF, progFd) < 0 ||
            ioctl(tpFd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
            logger.Warn("风驰: eBPF attach 失败");
            cleanup();
            return false;
        }

        ready = true;
        logger.Info("风驰: eBPF 事件源已启用");

        // MD 文件监听 (包名热更新, 失败不致命)
        mdFd = inotify_init();
        if (mdFd >= 0) {
            mdWd = inotify_add_watch(mdFd, "/storage/emulated/0/Android/CTS/game_packages.md",
                                     IN_MODIFY | IN_CLOSE_WRITE);
            if (mdWd < 0) { close(mdFd); mdFd = -1; }
        }
        return true;
    }

    // 阻塞等待事件: >0 = 捕获 pid; -1 = MD 文件变化; 0 = 无事件
    int WaitEvent() {
        struct pollfd fds[MAX_CPU + 1];
        int n = 0;
        for (int c = 0; c < MAX_CPU; c++) {
            fds[n].fd = perfFds[c];
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            n++;
        }
        if (mdFd >= 0) {
            fds[n].fd = mdFd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            n++;
        }
        const int pr = poll(fds, n, -1);
        if (pr <= 0) return 0;

        if (mdFd >= 0 && (fds[n - 1].revents & POLLIN)) {
            char buf[4096];
            read(mdFd, buf, sizeof(buf));
            return -1;
        }

        for (int c = 0; c < MAX_CPU; c++) {
            if (!(fds[c].revents & POLLIN)) continue;
            struct perf_event_mmap_page* mp = (struct perf_event_mmap_page*)bases[c];
            unsigned long long h = mp->data_head, t = mp->data_tail;
            if (h == t) continue;
            char* data = (char*)bases[c] + mp->data_offset;
            size_t dataSize = mp->data_size;
            unsigned int bad = 0;
            while (t < h && bad < 64) {   // 坏记录保护: 最多连续 64 条异常
                unsigned int off = (unsigned int)(t % dataSize);
                // 边界防护: 记录头(8) + raw_size(4) + pid(4) 必须完整落在 buffer 内
                if (off + 12 > dataSize) break;
                unsigned short recSize = *(unsigned short*)(data + off + 6);
                // 记录长度校验: 合理范围 [12, 4096], 否则视为坏记录丢弃整个 buffer
                if (recSize < 12 || recSize > 4096) {
                    bad++;
                    t = h;   // 丢弃剩余数据, 重新同步
                    mp->data_tail = t;
                    break;
                }
                unsigned int pid = *(unsigned int*)(data + off + 12);
                t += recSize;
                mp->data_tail = t;
                if (pid > 0) return (int)pid;
            }
        }
        return 0;
    }
};
