// ui.h —— 图形界面层（Win32 GDI 双缓冲）
// 精灵按 16×16 源图绘制；地图格 32×32（2 倍整数缩放）
#pragma once
#include <cstdint>
#include <string>
#include "sprites.h"

namespace ui {

// 初始化窗口（show=false 时隐藏，用于脚本自检）
bool init(bool show);
void shutdown();
bool active();
bool quitRequested();

// 画布布局
constexpr int MAP_Y = 40;         // 地图区顶部
constexpr int CELL = 32;          // 地图格像素
constexpr int MAP_COLS = 13;
constexpr int MAP_ROWS = 9;

// 文本颜色（ABGR 直接量）
constexpr uint32_t COL_DEFAULT = 0x00E8E9EA;
constexpr uint32_t COL_DIM     = 0x0088909C;
constexpr uint32_t COL_RED     = 0x00FF6B5E;
constexpr uint32_t COL_GREEN   = 0x007EE08A;
constexpr uint32_t COL_YELLOW  = 0x00FFD14A;
constexpr uint32_t COL_BLUE    = 0x009CC0FF;
constexpr uint32_t COL_MAGENTA = 0x00FF9EC4;
constexpr uint32_t COL_CYAN    = 0x004FD8D8;

// 精灵：一格（16×16 放大 2 倍，像素坐标）
void sprite(int px, int py, const spr::Sprite* s);
// 文本：布局坐标（col/row -> 像素的换算见 ui.cpp）
void text(int col, int row, uint32_t color, bool bold, const std::string& utf8);
// 覆盖标记（暗墙星 / 光标）
void overlayMark(int px, int py);
void cursorBox(int px, int py);
// 背景色
void bg(uint32_t rgb);
// 清空当前帧已积累的精灵与文本（终端 clear 的图形等价）
void clearFrame();
// 提交一帧（双缓冲交换 + 泵消息）
void present();
// 读键：先 present 再等待输入；返回统一键码（同 term::readKey）
int readKey();
// 导出当前帧为 BMP（24 位，bottom-up），用于自动校验
bool writeFrameBmp(const char* path);

} // namespace ui