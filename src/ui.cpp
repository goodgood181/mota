// ui.cpp —— Win32 GDI 图形层实现
#include "ui.h"
#include <windows.h>
#include <algorithm>
#include <deque>
#include <map>
#include <vector>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "msimg32.lib")

namespace ui {

static bool g_active = false;
static bool g_quit = false;
static HWND g_hwnd = nullptr;
static RECT g_rc = { 0, 0, 1272, 680 };

// 双缓冲
static HDC g_backDC = nullptr;
static HBITMAP g_backBmp = nullptr;

// 精灵缓存：32×32（2 倍整数缩放），带 Alpha
static std::map<const spr::Sprite*, HBITMAP> g_sprBmp;
static std::map<const spr::Sprite*, HDC>    g_sprDC;

// 字体
static HFONT g_font = nullptr;
static HFONT g_fontBold = nullptr;

// 帧数据
struct SpriteCmd { int px, py; const spr::Sprite* s; };
struct TextRun { int x, y; uint32_t color; bool bold; std::wstring w; };
static std::vector<SpriteCmd> g_sprites;
static std::vector<TextRun>   g_runs;
static uint32_t g_bg = 0x1B1D23;

// 输入
static std::deque<int> g_keys;

// ---------------- 布局换算 ----------------
constexpr int MAP_RIGHT = MAP_COLS * CELL;              // 416
constexpr int PANEL_X   = MAP_RIGHT + 16;               // 右侧面板起点
constexpr int PANEL_COLW = 24;                          // 面板列距
static int colToX(int col) {
    if (col == 0) return 0;
    if (col >= 13) return PANEL_X + (col - 13) * PANEL_COLW;
    return col * PANEL_COLW;
}
static int rowToY(int row) {
    if (row == 0) return 12;
    if (row <= 9) return MAP_Y + (row - 1) * CELL;
    return MAP_Y + MAP_ROWS * CELL + 6 + (row - 10) * 28;
}

// ---------------- 精灵缓存构建 ----------------
static void buildSpriteImage(const spr::Sprite* s) {
    HDC ref = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(ref);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 32;
    bi.bmiHeader.biHeight = -32;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto* px = (uint32_t*)bits;
    for (int sy = 0; sy < 16; sy++) {
        const std::string& row = s->rows[sy];
        for (int sx = 0; sx < 16 && sx < (int)row.size(); sx++) {
            char c = row[sx];
            auto it = spr::palette().find(c);
            uint32_t col = 0;
            if (it != spr::palette().end())
                // DIB 32bpp 内存序为 B,G,R,A
                col = 0xFF000000u | (it->second.r << 16) | (it->second.g << 8) | it->second.b;
            // 2 倍整数放大
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++)
                    px[(sy * 2 + dy) * 32 + (sx * 2 + dx)] = col;
        }
    }
    SelectObject(dc, bmp);
    g_sprBmp[s] = bmp;
    g_sprDC[s] = dc;
    ReleaseDC(nullptr, ref);
}

// ---------------- 公共 API ----------------
bool init(bool show) {
    if (g_active) return true;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        switch (m) {
            case WM_KEYDOWN: {
                int k = 0;
                switch (w) {
                    case VK_UP: k = 0x11; break;
                    case VK_DOWN: k = 0x12; break;
                    case VK_LEFT: k = 0x13; break;
                    case VK_RIGHT: k = 0x14; break;
                    case VK_RETURN: k = 0x0D; break;
                    case VK_ESCAPE: k = 0x1B; break;
                    case VK_SPACE: k = ' '; break;
                }
                if (k) g_keys.push_back(k);
                return 0;
            }
            case WM_CHAR: {
                wchar_t ch = (wchar_t)w;
                if (ch >= 32) g_keys.push_back((int)ch);
                return 0;
            }
            case WM_CLOSE:
                g_quit = true;
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(h, &ps);
                EndPaint(h, &ps);
                return 0;
            }
            default:
                return DefWindowProcW(h, m, w, l);
        }
    };
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)MAKEINTRESOURCE(32512));
    wc.lpszClassName = L"LunhuiTower";
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"《轮回之塔》 Loop Tower",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             g_rc.right, g_rc.bottom, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return false;
    GetClientRect(g_hwnd, &g_rc);
    if (show) ShowWindow(g_hwnd, SW_SHOW);

    // 双缓冲
    HDC ref = GetDC(g_hwnd);
    g_backDC = CreateCompatibleDC(ref);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_rc.right;
    bi.bmiHeader.biHeight = -g_rc.bottom;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_backBmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(g_backDC, g_backBmp);

    // 字体
    g_font = CreateFontW(-20, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    g_fontBold = CreateFontW(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");

    // 精灵缓存
    auto cacheAll = [](const spr::Sprite* s) { buildSpriteImage(s); };
    cacheAll(&spr::spr_floor); cacheAll(&spr::spr_floor_dark);
    cacheAll(&spr::spr_wall); cacheAll(&spr::spr_wall_h);
    cacheAll(&spr::spr_door1); cacheAll(&spr::spr_door2); cacheAll(&spr::spr_door3);
    cacheAll(&spr::spr_stair_up); cacheAll(&spr::spr_stair_down);
    cacheAll(&spr::spr_trigger); cacheAll(&spr::spr_mark);
    cacheAll(&spr::spr_hero); cacheAll(&spr::spr_npc_oldman);
    cacheAll(&spr::spr_npc_merchant); cacheAll(&spr::spr_npc_shade);
    cacheAll(&spr::spr_slime_green); cacheAll(&spr::spr_slime_split);
    cacheAll(&spr::spr_slime_blue); cacheAll(&spr::spr_bat);
    cacheAll(&spr::spr_skeleton); cacheAll(&spr::spr_wraith);
    cacheAll(&spr::spr_vampire); cacheAll(&spr::spr_golem); cacheAll(&spr::spr_guard);
    cacheAll(&spr::spr_key_y); cacheAll(&spr::spr_key_b); cacheAll(&spr::spr_key_r);
    cacheAll(&spr::spr_hp_small); cacheAll(&spr::spr_hp_big);
    cacheAll(&spr::spr_atk); cacheAll(&spr::spr_def); cacheAll(&spr::spr_gold);
    cacheAll(&spr::spr_fragment); cacheAll(&spr::spr_relic);

    ReleaseDC(g_hwnd, ref);
    g_active = true;
    return true;
}

void shutdown() {
    if (!g_active) return;
    for (auto& kv : g_sprBmp) DeleteObject(kv.second);
    g_sprBmp.clear(); g_sprDC.clear();
    if (g_backBmp) DeleteObject(g_backBmp);
    if (g_backDC) DeleteDC(g_backDC);
    if (g_font) DeleteObject(g_font);
    if (g_fontBold) DeleteObject(g_fontBold);
    if (g_hwnd) DestroyWindow(g_hwnd);
    g_active = false;
}

bool active() { return g_active; }
bool quitRequested() { return g_quit; }

void bg(uint32_t rgb) { g_bg = rgb; }

void clearFrame() {
    g_sprites.clear();
    g_runs.clear();
}

void sprite(int px, int py, const spr::Sprite* s) {
    if (!s) return;
    g_sprites.push_back({ px, py, s });
}

void overlayMark(int px, int py) { sprite(px, py, &spr::spr_mark); }
void cursorBox(int px, int py) { sprite(px, py, &spr::spr_mark); } // 复用星标作光标

static std::wstring toWide(const std::string& u8) {
    if (u8.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), &w[0], n);
    return w;
}

void text(int col, int row, uint32_t color, bool bold, const std::string& utf8) {
    if (utf8.empty()) return;
    int x = colToX(col), y = rowToY(row);
    // 调试：检测“本帧有精灵 + 文本落入地图区”的重叠
    if (getenv("MOTA_OVERLAP")) {
        if (!g_sprites.empty() && x < MAP_RIGHT + 8 && y >= MAP_Y - 2 && y < MAP_Y + MAP_ROWS * CELL)
            fprintf(stderr, "[overlap] col=%d row=%d x=%d y=%d text=%.30s\n", col, row, x, y, utf8.c_str());
    }
    g_runs.push_back({ x, y, color, bold, toWide(utf8) });
}

void present() {
    if (!g_active) { g_sprites.clear(); g_runs.clear(); return; }
    // 调试：检测空白帧（无精灵也无文本即上屏）
    if (getenv("MOTA_BLANK")) {
        if (g_sprites.empty() && g_runs.empty())
            fprintf(stderr, "[BLANK frame]\n");
    }
    HDC dc = g_backDC;
    // 背景
    RECT rc{ 0, 0, g_rc.right, g_rc.bottom };
    HBRUSH br = CreateSolidBrush(RGB((g_bg >> 16) & 0xFF, (g_bg >> 8) & 0xFF, g_bg & 0xFF));
    FillRect(dc, &rc, br);
    DeleteObject(br);
    // 精灵（Alpha 混合）
    for (auto& c : g_sprites) {
        auto it = g_sprDC.find(c.s);
        if (it == g_sprDC.end()) continue;
        BLENDFUNCTION bf{};
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;
        AlphaBlend(dc, c.px, c.py, CELL, CELL, it->second, 0, 0, 32, 32, bf);
    }
    // 文字
    for (auto& r : g_runs) {
        SelectObject(dc, r.bold ? g_fontBold : g_font);
        SetTextColor(dc, RGB((r.color >> 16) & 0xFF, (r.color >> 8) & 0xFF, r.color & 0xFF));
        SetBkMode(dc, TRANSPARENT);
        TextOutW(dc, r.x, r.y, r.w.c_str(), (int)r.w.size());
    }
    // 上屏
    HDC wnd = GetDC(g_hwnd);
    BitBlt(wnd, 0, 0, g_rc.right, g_rc.bottom, dc, 0, 0, SRCCOPY);
    ReleaseDC(g_hwnd, wnd);
    g_sprites.clear();
    g_runs.clear();
    // 泵消息（保持窗口响应）
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

int readKey() {
    present();
    if (g_quit) return 0x1B;
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_quit = true; return 0x1B; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (!g_keys.empty()) { int k = g_keys.front(); g_keys.pop_front(); return k; }
            if (g_quit) return 0x1B;
        }
        if (!g_keys.empty()) { int k = g_keys.front(); g_keys.pop_front(); return k; }
        WaitMessage();
    }
}

// 名称 -> 精灵（供渲染层查表）
const spr::Sprite* spriteByName(const std::string& name) {
    return spr::byName(name);
}

// 把当前后备缓冲导出为 24 位 BMP（用于自动校验渲染）
bool writeFrameBmp(const char* path) {
    if (!g_active) return false;
    int w = g_rc.right, h = g_rc.bottom;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> px(w * h * 3);
    if (!GetDIBits(g_backDC, g_backBmp, 0, h, px.data(), &bi, DIB_RGB_COLORS))
        return false;
    int rowBytes = (w * 3 + 3) & ~3;
    std::vector<uint8_t> file(54 + rowBytes * h, 0);
    file[0] = 'B'; file[1] = 'M';
    uint32_t size = (uint32_t)file.size();
    memcpy(&file[2], &size, 4);
    uint32_t off = 54;
    memcpy(&file[10], &off, 4);
    uint32_t header = 40;
    memcpy(&file[14], &header, 4);
    int32_t w32 = w, h32 = h;
    memcpy(&file[18], &w32, 4);
    memcpy(&file[22], &h32, 4);
    uint16_t planes = 1; memcpy(&file[26], &planes, 2);
    uint16_t bpp = 24;  memcpy(&file[28], &bpp, 2);
    for (int y = 0; y < h; y++) {
        int row = h - 1 - y; // 自底向上
        memcpy(&file[54 + row * rowBytes], &px[y * w * 3], (size_t)w * 3);
    }
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return true;
}

} // namespace ui