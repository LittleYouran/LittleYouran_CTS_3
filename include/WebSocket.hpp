#pragma once

#include "Utils.hpp"
#include "Logger.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

static constexpr const char* WS_KEY = "LittleYouranCTS31415";

// ---- SHA-1 (RFC 3174) ----
struct Sha1Ctx {
    unsigned int h[5];
    unsigned long long len;
    unsigned char buf[64];
    size_t buflen;
};

static inline unsigned int rotl32(unsigned int v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_init(Sha1Ctx* ctx) {
    ctx->h[0] = 0x67452301; ctx->h[1] = 0xEFCDAB89;
    ctx->h[2] = 0x98BADCFE; ctx->h[3] = 0x10325476; ctx->h[4] = 0xC3D2E1F0;
    ctx->len = 0; ctx->buflen = 0;
}

static void sha1_block(Sha1Ctx* ctx, const unsigned char* p) {
    unsigned int w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned int)p[i*4] << 24) | ((unsigned int)p[i*4+1] << 16) |
               ((unsigned int)p[i*4+2] << 8) | (unsigned int)p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = rotl32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    unsigned int a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3], e = ctx->h[4];
    for (int i = 0; i < 80; i++) {
        unsigned int f, k;
        if (i < 20)      { f = (b & c) | (~b & d);       k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                k = 0xCA62C1D6; }
        unsigned int tmp = rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = tmp;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d; ctx->h[4] += e;
}

static void sha1_update(Sha1Ctx* ctx, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    ctx->len += n;
    while (n > 0) {
        size_t take = 64 - ctx->buflen;
        if (take > n) take = n;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take; p += take; n -= take;
        if (ctx->buflen == 64) { sha1_block(ctx, ctx->buf); ctx->buflen = 0; }
    }
}

static void sha1_final(Sha1Ctx* ctx, unsigned char out[20]) {
    unsigned long long bitlen = ctx->len * 8;
    unsigned char pad = 0x80;
    sha1_update(ctx, &pad, 1);
    unsigned char zero = 0;
    while (ctx->buflen != 56) sha1_update(ctx, &zero, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bitlen >> (56 - i*8));
    sha1_update(ctx, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (unsigned char)(ctx->h[i] >> 24);
        out[i*4+1] = (unsigned char)(ctx->h[i] >> 16);
        out[i*4+2] = (unsigned char)(ctx->h[i] >> 8);
        out[i*4+3] = (unsigned char)ctx->h[i];
    }
}

// ---- Base64 ----
static void base64_encode(const unsigned char* in, size_t n, char* out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;
    while (i + 2 < n) {
        unsigned int v = (in[i] << 16) | (in[i+1] << 8) | in[i+2];
        out[o++] = tbl[(v >> 18) & 63]; out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63];  out[o++] = tbl[v & 63];
        i += 3;
    }
    if (i + 1 == n) {
        unsigned int v = in[i] << 16;
        out[o++] = tbl[(v >> 18) & 63]; out[o++] = tbl[(v >> 12) & 63];
        out[o++] = '='; out[o++] = '=';
    } else if (i + 2 == n) {
        unsigned int v = (in[i] << 16) | (in[i+1] << 8);
        out[o++] = tbl[(v >> 18) & 63]; out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63];  out[o++] = '=';
    }
    out[o] = '\0';
}

// ---- WebSocket 服务器 ----
class WebSocketServer {
private:
    static constexpr int WS_PORT = 31415;
    static constexpr const char* LOG_PATH = "/sdcard/Android/CTS/log.txt";
    static constexpr const char* MODE_PATH = "/sdcard/Android/CTS/mode.txt";
    static constexpr const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    Utils utils;
    Logger logger;
    bool running = false;
    std::streamoff logLastPos = 0; 

    static bool wsSend(int fd, const char* data, size_t len) {
        if (len > 65535) return false;
        unsigned char hdr[10];
        size_t hlen = 2;
        hdr[0] = 0x81;  // FIN + text
        if (len < 126) {
            hdr[1] = (unsigned char)len;
        } else {
            hdr[1] = 126;
            hdr[2] = (unsigned char)(len >> 8);
            hdr[3] = (unsigned char)(len & 0xFF);
            hlen = 4;
        }
        if (write(fd, hdr, hlen) != (ssize_t)hlen) return false;
        return write(fd, data, len) == (ssize_t)len;
    }

    static bool wsSendStr(int fd, const std::string& s) {
        return wsSend(fd, s.data(), s.size());
    }

    static bool wsReadFrame(int fd, std::string& text) {
        unsigned char hdr[2];
        ssize_t n = read(fd, hdr, 2);
        if (n != 2) return false;

        int opcode = hdr[0] & 0x0F;
        bool masked = hdr[1] & 0x80;
        unsigned long long plen = hdr[1] & 0x7F;

        if (plen == 126) {
            unsigned char ext[2];
            if (read(fd, ext, 2) != 2) return false;
            plen = ((unsigned long long)ext[0] << 8) | ext[1];
        } else if (plen == 127) {
            unsigned char ext[8];
            if (read(fd, ext, 8) != 8) return false;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
        }

        if (plen > 1024 * 1024) return false;  // 限制 1MB

        unsigned char mask[4] = { 0, 0, 0, 0 };
        if (masked) {
            if (read(fd, mask, 4) != 4) return false;
        }

        std::string payload;
        payload.resize(plen);
        size_t got = 0;
        while (got < plen) {
            ssize_t r = read(fd, &payload[got], plen - got);
            if (r <= 0) return false;
            got += (size_t)r;
        }

        if (masked) {
            for (size_t i = 0; i < plen; i++) payload[i] ^= mask[i % 4];
        }

        if (opcode == 0x8) return false;   // close
        if (opcode == 0x9) {               // ping → pong
            unsigned char pong[2] = { 0x8A, 0x00 };
            write(fd, pong, 2);
            return true;
        }
        if (opcode == 0xA) return true;    // pong
        if (opcode == 0x1 || opcode == 0x2) {  // text / binary
            text = payload;
            return true;
        }
        return true;  // 未知帧忽略
    }

    // 读取并解析 HTTP 握手, 返回请求路径或空
    static std::string readHandshake(int fd, std::string& outKey, bool& keyOk) {
        keyOk = false;
        char buf[4096];
        size_t got = 0;
        while (got < sizeof(buf) - 1) {
            ssize_t r = read(fd, buf + got, sizeof(buf) - 1 - got);
            if (r <= 0) return "";
            got += (size_t)r;
            if (got >= 4 && memcmp(buf + got - 4, "\r\n\r\n", 4) == 0) break;
            if (got >= 2 && memcmp(buf + got - 2, "\n\n", 2) == 0) break;
        }
        buf[got] = '\0';

        std::string path;
        char* line = buf;
        if (strncmp(line, "GET ", 4) == 0) {
            char* sp = strchr(line + 4, ' ');
            if (sp) path.assign(line + 4, sp - (line + 4));
        }

        char* p = buf;
        while ((p = strstr(p, "Sec-WebSocket-Protocol:")) != nullptr) {
            const char* v = p + 23;
            while (*v == ' ') v++;
            char* e = strchr((char*)v, '\r');
            if (!e) e = strchr((char*)v, '\n');
            std::string proto = e ? std::string(v, e - v) : std::string(v);
            if (proto == WS_KEY) keyOk = true;
            p = e ? e : p + 1;
        }
        p = buf;
        while ((p = strstr(p, "Sec-WebSocket-Key:")) != nullptr) {
            const char* v = p + 18;
            while (*v == ' ') v++;
            char* e = strchr((char*)v, '\r');
            if (!e) e = strchr((char*)v, '\n');
            outKey = e ? std::string(v, e - v) : std::string(v);
            break;
        }
        return path;
    }

    static bool sendHandshake(int fd, const std::string& key, bool keyOk) {
        std::string accept;
        {
            std::string raw = key + WS_GUID;
            Sha1Ctx ctx;
            sha1_init(&ctx);
            sha1_update(&ctx, raw.data(), raw.size());
            unsigned char digest[20];
            sha1_final(&ctx, digest);
            char b64[64];
            base64_encode(digest, 20, b64);
            accept = b64;
        }
        char resp[512];
        int n = snprintf(resp, sizeof(resp),
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n"
            "Sec-WebSocket-Protocol: %s\r\n"
            "\r\n",
            accept.c_str(),
            keyOk ? WS_KEY : "none");
        return write(fd, resp, (size_t)n) == n;
    }

    static bool wsSendChunked(int fd, const std::string& data) {
        const size_t CHUNK = 32768;
        size_t off = 0;
        while (off < data.size()) {
            const size_t n = (data.size() - off < CHUNK) ? (data.size() - off) : CHUNK;
            if (!wsSend(fd, data.data() + off, n)) return false;
            off += n;
        }
        return true;
    }

    void handleLogs(int fd) {
        // 推送现有内容 (分块)
        std::ifstream ifs(LOG_PATH, std::ios::binary);
        if (ifs) {
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            if (!content.empty()) {
                if (!wsSendChunked(fd, content)) { close(fd); return; }
            }
        }

        // inotify 监听增量 (位置为连接局部, 多连接互不干扰)
        const int inot = inotify_init();
        if (inot < 0) { close(fd); return; }
        const int wd = inotify_add_watch(inot, LOG_PATH, IN_MODIFY);
        if (wd < 0) { close(inot); close(fd); return; }

        std::streamoff lastPos = 0;
        char buf[8192];
        std::string pending;

        // poll 同时监听 inotify 与 socket: 客户端断开时能退出并释放 fd
        struct pollfd fds[2];
        fds[0].fd = inot;
        fds[0].events = POLLIN;
        fds[1].fd = fd;
        fds[1].events = POLLIN | POLLHUP | POLLERR;

        while (true) {
            const int pr = poll(fds, 2, -1);
            if (pr < 0) break;

            // socket 有事件: 断开(FIN/EOF/close帧) → 退出; ping → pong
            if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
                std::string dummy;
                if (!wsReadFrame(fd, dummy)) break;   // close帧/EOF → 退出
            }

            // inotify 事件: 读日志增量推送
            if (fds[0].revents & POLLIN) {
                const ssize_t len = read(inot, buf, sizeof(buf));
                if (len <= 0) continue;

                // 读新增内容 (从文件尾部)
                std::ifstream ifs2(LOG_PATH, std::ios::binary | std::ios::ate);
                if (!ifs2) break;
                const std::streamoff size = ifs2.tellg();
                if (size < lastPos) lastPos = 0;   // 文件被清空
                if (size > lastPos) {
                    ifs2.seekg(lastPos);
                    std::string chunk((std::istreambuf_iterator<char>(ifs2)), std::istreambuf_iterator<char>());
                    lastPos = size;
                    pending += chunk;
                    // 按行推送
                    size_t nl;
                    while ((nl = pending.find('\n')) != std::string::npos) {
                        std::string line = pending.substr(0, nl + 1);
                        pending.erase(0, nl + 1);
                        if (!wsSendStr(fd, line)) { close(inot); close(fd); return; }
                    }
                }
            }
        }
        inotify_rm_watch(inot, wd);
        close(inot);
        close(fd);
    }

    // /modes 处理: 接收模式 + 推送状态
    // 推送当前模式 (仅事件触发时调用, 无周期性轮询)
    bool sendMode(int fd) {
        char mbuf[64] = { 0 };
        utils.readString(MODE_PATH, mbuf, sizeof(mbuf) - 1);
        mbuf[sizeof(mbuf) - 1] = '\0';
        std::string mode = mbuf;
        while (!mode.empty() && (mode.back() == '\n' || mode.back() == '\r' ||
               mode.back() == ' ' || mode.back() == '\t')) mode.pop_back();
        if (mode.empty()) mode = "unknown";

        char out[96];
        snprintf(out, sizeof(out), "mode:%s", mode.c_str());
        return wsSendStr(fd, out);
    }

    void handleModes(int fd) {
        // 初始推送一次当前模式
        if (!sendMode(fd)) { close(fd); return; }

        while (true) {
            // 纯事件驱动: 无限阻塞等待模式切换指令 (空闲时 0 CPU)
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            const int pr = poll(&pfd, 1, -1);
            if (pr < 0) break;

            if (pr > 0 && (pfd.revents & POLLIN)) {
                std::string msg;
                if (!wsReadFrame(fd, msg)) break;
                if (!msg.empty()) {
                    // 校验白名单模式并写入 mode.txt
                    if (msg == "powersave" || msg == "balance" ||
                        msg == "performance" || msg == "fast") {
                        utils.WriteFile(MODE_PATH, msg.c_str());
                        logger.Info("WebUI: 模式切换为 %s", msg.c_str());
                    }
                }
                // 推送当前模式
                if (!sendMode(fd)) break;
            }
        }
        close(fd);
    }

    // 连接处理入口
    void handleConnection(int fd) {
        std::string key;
        bool keyOk = false;
        const std::string path = readHandshake(fd, key, keyOk);

        if (path.empty() || key.empty() || !keyOk) {
            logger.Warn("WebUI: 握手失败 (路径=%s 密钥=%s)", path.c_str(), keyOk ? "OK" : "错误");
            close(fd);
            return;
        }

        if (!sendHandshake(fd, key, keyOk)) {
            close(fd);
            return;
        }

        logger.Debug("WebUI: 已连接 %s", path.c_str());

        if (path == "/logs") {
            handleLogs(fd);
        } else if (path == "/modes") {
            handleModes(fd);
        } else {
            close(fd);
        }

        logger.Debug("WebUI: %s 连接已断开", path.c_str());
    }

    // accept 循环
    void serverLoop() {
        const int listenFd = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd < 0) {
            logger.Error("WebUI: 创建 socket 失败");
            return;
        }

        int opt = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 仅本机
        addr.sin_port = htons(WS_PORT);

        if (bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            logger.Error("WebUI: 绑定端口 %d 失败", WS_PORT);
            close(listenFd);
            return;
        }

        if (listen(listenFd, 8) < 0) {
            logger.Error("WebUI: listen 失败");
            close(listenFd);
            return;
        }

        logger.Info("WebUI: 服务已启动 端口 %d", WS_PORT);

        while (running) {
            try {
                struct sockaddr_in client;
                socklen_t clen = sizeof(client);
                const int cfd = accept(listenFd, (struct sockaddr*)&client, &clen);
                if (cfd < 0) continue;
                std::thread([this, cfd]() {
                    try {
                        handleConnection(cfd);
                    } catch (...) {
                        close(cfd);
                    }
                }).detach();
            } catch (...) {
                // 线程创建失败等异常: 不影响主循环
            }
        }

        close(listenFd);
    }

public:
    void Start() {
        if (running) return;
        running = true;
        std::thread(&WebSocketServer::serverLoop, this).detach();
    }

    void Stop() {
        running = false;
    }
};