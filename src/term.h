// term.h —— 终端/输入/文本工具（Windows 优先，兼顾 ANSI 终端；含图形模式桥接）
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <deque>
#include <vector>
#include "ui.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <conio.h>
#endif

namespace term {

inline bool g_vt = false;
inline int  g_rows = 30;
inline int  g_cols = 100;

// 脚本化按键队列（--script 冒烟用）：非空时 readKey 从这里取，避免阻塞终端
inline std::deque<int> g_scriptKeys;

// 初始化终端：UTF-8 代码页 + 启用 ANSI 转义
inline void init() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(hOut, &mode)) {
        if (SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            g_vt = true;
        }
    }
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(hOut, &info)) {
        g_cols = info.srWindow.Right - info.srWindow.Left + 1;
        g_rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
#endif
}

// ---- 图形模式（--gui 默认开）：print 走 ui 文本层 ----
inline bool g_gui = false;
inline int  g_cx = 0, g_cy = 0;            // 文本光标（布局坐标）
inline uint32_t g_fg = ui::COL_DEFAULT;
inline bool g_fgBold = false;

inline uint32_t ansiColor(int c) {
    switch (c) {
        case 31: case 91: return ui::COL_RED;
        case 32: case 92: return ui::COL_GREEN;
        case 33: case 93: return ui::COL_YELLOW;
        case 34: case 94: return ui::COL_BLUE;
        case 35: case 95: return ui::COL_MAGENTA;
        case 36: case 96: return ui::COL_CYAN;
        case 30: case 90: case 37: case 97: return ui::COL_DEFAULT;
        case 2: return ui::COL_DIM;
        default: return g_fg;
    }
}

inline void print(const std::string& s) {
    if (!g_gui) { std::fputs(s.c_str(), stdout); std::fflush(stdout); return; }
    // 解析 ANSI 转义 + 换行，逐段送往 ui 文本层
    std::string cur;
    size_t i = 0;
    auto flush = [&]() {
        if (cur.empty()) return;
        if (cur.find('\n') == std::string::npos) {
            ui::text(g_cx, g_cy, g_fg, g_fgBold, cur);
            for (unsigned char c : cur) g_cx += (c < 0x80) ? 1 : 2;
        } else {
            // 含换行：逐行输出
            size_t pos = 0;
            while (pos <= cur.size()) {
                size_t nl = cur.find('\n', pos);
                std::string line = (nl == std::string::npos) ? cur.substr(pos) : cur.substr(pos, nl - pos);
                ui::text(g_cx, g_cy, g_fg, g_fgBold, line);
                for (unsigned char c : line) g_cx += (c < 0x80) ? 1 : 2;
                if (nl == std::string::npos) break;
                g_cx = 0; g_cy++;
                pos = nl + 1;
            }
        }
        cur.clear();
    };
    while (i < s.size()) {
        char c = s[i];
        if (c == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            flush();
            size_t j = i + 2;
            while (j < s.size() && !(s[j] >= '@' && s[j] <= '~')) j++;
            if (j < s.size()) {
                std::string param = s.substr(i + 2, j - i - 2);
                char termCh = s[j];
                int code = 0;
                try { code = std::stoi(param); } catch (...) { code = 0; }
                if (termCh == 'm') {
                    if (code == 0) { g_fg = ui::COL_DEFAULT; g_fgBold = false; }
                    else if (code == 1) g_fgBold = true;
                    else g_fg = ansiColor(code);
                }
            }
            i = (j < s.size()) ? j + 1 : s.size();
        } else {
            cur += c;
            i++;
        }
    }
    flush();
}

// ANSI 颜色（VT 模式下生效）
inline std::string col(int code) {
    if (!g_vt) return "";
    char buf[24];
    std::snprintf(buf, sizeof buf, "\x1b[%dm", code);
    return buf;
}
inline std::string reset() { return col(0); }
inline std::string fg(int c) { return col(30 + c); }   // 0黑 1红 2绿 3黄 4蓝 5紫 6青 7白
inline std::string bg(int c) { return col(40 + c); }
inline std::string bold() { return col(1); }
inline std::string dim() { return col(2); }

inline void clear() {
    if (g_gui) { ui::clearFrame(); g_cx = 0; g_cy = 0; return; }
    if (g_vt) print("\x1b[2J\x1b[H");
    else print("\n\n\n\n\n\n\n\n\n\n");
}
inline void setpos(int x, int y) {
    if (g_gui) { g_cx = x; g_cy = y; return; }
    if (!g_vt) return;
    char buf[24];
    std::snprintf(buf, sizeof buf, "\x1b[%d;%dH", y + 1, x + 1);
    print(buf);
}
inline void cursor(int show) {
    if (g_gui) return;
    if (!g_vt) return;
    print(show ? "\x1b[?25h" : "\x1b[?25l");
}

// 计算字符串的显示宽度（CJK 占 2 列；按 UTF-8 字符边界统计，与 wrapText 口径一致）
inline int dispWidth(const std::string& s) {
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        int len = 1;
        if (c >= 0xF0) len = 4;          // 4 字节（罕见）
        else if (c >= 0xE0) len = 3;     // 3 字节（汉字）
        else if (c >= 0xC0) len = 2;     // 2 字节
        w += (c < 0x80) ? 1 : 2;
        i += (size_t)len;
    }
    return w;
}

// 按“显示宽度”（CJK=2 列）自动换行，返回各行（UTF-8 安全，按字符边界切分）
inline std::vector<std::string> wrapText(const std::string& s, int maxCols) {
    std::vector<std::string> out;
    std::string cur;
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        int len = 1;
        if (c >= 0xF0) len = 4;          // 4 字节（罕见）
        else if (c >= 0xE0) len = 3;     // 3 字节（汉字）
        else if (c >= 0xC0) len = 2;     // 2 字节
        int cw = (c < 0x80) ? 1 : 2;     // 显示列宽：ASCII 1 列，其余按 2 列
        if (w + cw > maxCols && !cur.empty()) {
            out.push_back(cur);
            cur.clear();
            w = 0;
        }
        cur.append(s, i, (size_t)len);
        w += cw;
        i += (size_t)len;
    }
    if (!cur.empty() || out.empty()) out.push_back(cur);
    return out;
}
// 按显示宽度右/左补齐
inline std::string pad(const std::string& s, int width) {
    int w = dispWidth(s);
    std::string out = s;
    while (w < width) { out += ' '; w++; }
    return out;
}
inline std::string padL(const std::string& s, int width) {
    int w = dispWidth(s);
    std::string out;
    while (w < width) { out += ' '; w++; }
    return out + s;
}

// 读取一个键：返回统一编码
// 返回: 方向键(上=0x11 下=0x12 左=0x13 右=0x14)，普通字符原样，enter=0x0D，esc=0x1B
inline int readKey() {
    if (!g_scriptKeys.empty()) {
        // 调试钩子（MOTA_BLANK=1 时脚本模式与真实模式一致：每次读键先上屏，
        // 让空白帧检测覆盖真实按键路径。脚本本身语义不变。）
        if (getenv("MOTA_BLANK") && g_gui && ui::active()) ui::present();
        int k = g_scriptKeys.front();
        g_scriptKeys.pop_front();
        return k;
    }
    if (g_gui && ui::active()) return ui::readKey();
#ifdef _WIN32
    int c = _getch();
    if (c == 0 || c == 0xE0) {          // 扩展键
        int c2 = _getch();
        switch (c2) {
            case 72: return 0x11;
            case 80: return 0x12;
            case 75: return 0x13;
            case 77: return 0x14;
            default: return c2;
        }
    }
    return c;
#else
    // 简易 POSIX 回退（一般不会用到）
    int c = std::getchar();
    return c;
#endif
}

// 阻塞直到按任意键
inline void anyKey(const std::string& prompt = "按任意键继续...") {
    print("\n" + dim() + prompt + reset());
    readKey();
}

// 睡眠毫秒
inline void sleepMs(int ms) {
    if (g_gui && ui::active()) ui::present();
#ifdef _WIN32
    Sleep(ms);
#endif
}

} // namespace term