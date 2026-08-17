#pragma once

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

typedef unsigned long long u64;

#define INLINE __attribute__((always_inline)) inline

namespace LibUtils {
    static INLINE bool startWith(const char* str, const char* prefix) {
        while (*prefix) {
            if (*str++ != *prefix++)
                return false;
        }
        return true;
    }

    static INLINE constexpr int fast_abs(int x) noexcept {
        return x < 0 ? -x : x;
    }

    static INLINE char* lastChar(char* ptr) {
        if (ptr == nullptr) return nullptr;
        while (*ptr) ptr++;
        return ptr - 1;
    }
    
    static INLINE int Fastatoi(const char* str) {
        bool negative = false;
        if (*str == '-') {
            negative = true;
            str++;
        }
        int num = 0;
        while (*str >= '0' && *str <= '9') {
            num = num * 10 + (*str++ - '0');
        }
        return negative ? -num : num;
    }

    static INLINE size_t Faststrlen(const char* str) {
        size_t size = 0;
        while (str[size] != '\0') {
            size++;
        }
        return size;
    }

    static INLINE char* emit_u32(char *buf, uint32_t val) {
        char tmp[10];
        char *out = tmp + sizeof(tmp);
    
        do {
            *--out = (char)('0' + (val % 10u));
            val /= 10;
        } while (val);

        const size_t len = (size_t)(tmp + sizeof(tmp) - out);
        memcpy(buf, out, len);
        return buf + len;
    }

    static INLINE int FastVsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
        if (buf == nullptr || size == 0) return 0;
        char* p = buf;
        char* end = buf + size - 1;
    
        while (*fmt) {
            if (*fmt != '%') {
                if (p != end) *p = *fmt;
                ++p; ++fmt;
                continue;
            }
            ++fmt;                          
            switch (*fmt++) {
            case 'd': {
                int v = va_arg(ap, int);
                if (v < 0) {
                    if (p != end) *p++ = '-';
                    p = emit_u32(p, (uint32_t)(-(unsigned)v));
                } else {
                    p = emit_u32(p, (uint32_t)v);
                }
                break;
            }

            case 'u': {
                int v = va_arg(ap, int);
                p = emit_u32(p, (uint32_t)v);
                break;
            }

            case 's': {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                while (*s && p != end) {
                    *p++ = *s++;
                }
                break;
            }

            case '%':
                if (p != end) *p++ = '%';
                break;
            }
        }

        if (p > end) *end = '\0'; else *p = '\0';
        return (int)(p - buf);              
    }
    
    static INLINE int FastSnprintf(char* buf, size_t size, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        int r = FastVsnprintf(buf, size, fmt, ap);
        va_end(ap);
        return r;
    }

    static INLINE const char* itoa(const int num) {
        static char buf[11];               

        char* p = buf;

        if (num < 0) return "0"; 

        p = emit_u32(p, num);
        *p = '\0';

        return buf;
    }
};