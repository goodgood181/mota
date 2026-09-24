// types.h —— 公共基础类型
#pragma once
#include <string>

// 三色钥匙：0黄 1蓝 2红
enum KeyColor { KEY_YELLOW = 0, KEY_BLUE = 1, KEY_RED = 2 };
inline const char* keyName(int k) {
    static const char* names[] = { "黄钥匙", "蓝钥匙", "红钥匙" };
    return names[k];
}

// 玩家/怪物基础属性
struct Stats {
    int hp = 0;
    int atk = 0;
    int def = 0;
    int gold = 0;
    int exp = 0;
};

struct Point {
    int x = 0, y = 0;
    bool operator==(const Point& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Point& o) const { return !(*this == o); }
    bool operator<(const Point& o) const { return y < o.y || (y == o.y && x < o.x); }
    Point operator+(const Point& o) const { return { x + o.x, y + o.y }; }
};

// 方向增量（上 下 左 右）
inline Point dirDelta(int d) {
    switch (d) {
        case 0: return { 0, -1 }; // 上
        case 1: return { 0, 1 };  // 下
        case 2: return { -1, 0 }; // 左
        case 3: return { 1, 0 };  // 右
    }
    return { 0, 0 };
}
inline const char* dirName(int d) {
    static const char* n[] = { "上", "下", "左", "右" };
    return n[d];
}