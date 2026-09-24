// floorgame.h —— 单层地图运行时
// 地图文本约定的实体字符：
//   # 墙   . 地板   @ 玩家出生点   > 上楼   < 下楼
//   H 隐藏墙（看起来像墙，走进去才被发现）
//   1 黄门  2 蓝门  3 红门
//   n NPC（绑定 npc_event）  T 触发点（绑定 trigger_event）  s 商店
//   k/b 钥匙类道具由 items 配置：其余字母在 floors.json 的 monsters/items 中映射
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <ostream>
#include <istream>
#include "data.h"
#include "types.h"

struct MonsterInst {
    std::string id;
    bool alive = true;
    char ch = 'a'; // 显示字符（地图中的字母）
};
struct ItemInst {
    std::string id;
    bool taken = false;
    bool permanent = false; // 碎片等跨轮回保留
};

struct Floor {
    int w = 0, h = 0;
    std::vector<std::string> grid;      // 当前状态（门开过的位置变为 '=’）
    std::vector<std::string> origGrid;  // 初始状态（轮回重置用）
    std::map<Point, MonsterInst> monsters;
    std::map<Point, ItemInst> items;
    std::vector<std::vector<bool>> explored;  // 跨轮回累计
    std::map<Point, bool> markedWalls;        // 执念标记的暗墙（跨轮回）
    Point downStair{-1, -1}, upStair{-1, -1}; // 上/下楼位置（实际存在时）
    Point start{-1, -1};                      // 玩家出生点
    bool hasShop = false;
    std::string npcEvent, triggerEvent;
    std::string goal;                 // 主线指引（floors.json "goal"，可被 goalText 动态补充）
    std::set<char> monsterChars, itemChars;   // 用于渲染配色

    char tile(int x, int y) const {
        if (y < 0 || y >= h || x < 0 || x >= w) return '#';
        char c = grid[y][x];
        return c;
    }
    bool inBounds(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }

    // 是否阻挡移动
    bool solid(int x, int y) const {
        if (!inBounds(x, y)) return true;
        char c = grid[y][x];
        if (c == '#') return true;
        if (c == '1' || c == '2' || c == '3') return true; // 关着的门
        return false;
    }
    bool isHiddenWall(int x, int y) const { return inBounds(x, y) && grid[y][x] == 'H'; }
    bool isDoor(int x, int y) const {
        char c = tile(x, y);
        return c == '1' || c == '2' || c == '3';
    }
    int doorColor(int x, int y) const {
        char c = tile(x, y);
        if (c == '1') return KEY_YELLOW;
        if (c == '2') return KEY_BLUE;
        if (c == '3') return KEY_RED;
        return -1;
    }
    bool isStair(int x, int y) const {
        char c = tile(x, y);
        return c == '>' || c == '<' || ((Point{x, y} == upStair || Point{x, y} == downStair));
    }
    bool isShopTile(int x, int y) const { return tile(x, y) == 's'; }

    void markExplored(int x, int y) {
        if (inBounds(x, y) && x < (int)explored[y].size()) {
            explored[y][x] = true;
        }
    }
    bool isExplored(int x, int y) const {
        if (!inBounds(x, y)) return false;
        return x < (int)explored[y].size() && explored[y][x];
    }

    // 创建楼层
    static bool create(const FloorConfig& cfg, const GameData& data, Floor& out, std::string& err) {
        std::ifstream f(cfg.file, std::ios::binary);
        if (!f) { err = "无法打开地图文件: " + cfg.file; return false; }
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string text = ss.str();
        // 去掉 BOM
        if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
            text = text.substr(3);

        std::vector<std::string> rows;
        {
            std::string cur;
            for (char c : text) {
                if (c == '\n' || c == '\r') {
                    if (!cur.empty()) { rows.push_back(cur); cur.clear(); }
                } else cur += c;
            }
            if (!cur.empty()) rows.push_back(cur);
        }
        // 去掉首尾空行
        while (!rows.empty() && rows.front().empty()) rows.erase(rows.begin());
        while (!rows.empty() && rows.back().empty()) rows.pop_back();
        if (rows.empty()) { err = "地图为空: " + cfg.file; return false; }

        out.w = (int)rows[0].size();
        out.h = (int)rows.size();
        out.grid = rows;
        out.origGrid = rows;
        out.explored.assign(out.h, std::vector<bool>(out.w, false));
        out.hasShop = cfg.shop;
        out.npcEvent = cfg.npcEvent;
        out.triggerEvent = cfg.triggerEvent;
        out.goal = cfg.goal;

        for (auto& kv : cfg.monsters) out.monsterChars.insert(kv.first);
        for (auto& kv : cfg.items) out.itemChars.insert(kv.first);

        // 扫描实体
        for (int y = 0; y < out.h; y++) {
            const std::string& row = rows[y];
            if ((int)row.size() != out.w) {
                err = "地图行宽不一致 (行 " + std::to_string(y) + "): " + cfg.file;
                return false;
            }
            for (int x = 0; x < out.w; x++) {
                char c = row[x];
                Point p{x, y};
                if (c == '>' || c == '<') {
                    if (c == '>') out.upStair = p;
                    else out.downStair = p;
                    continue;
                }
                auto mit = cfg.monsters.find(c);
                if (mit != cfg.monsters.end()) {
                    out.monsters[p] = MonsterInst{mit->second, true, c};
                    continue;
                }
                auto iit = cfg.items.find(c);
                if (iit != cfg.items.end()) {
                    ItemInst ii{iit->second, false, false};
                    const ItemDef* def = data.item(ii.id);
                    if (def && def->type == "fragment") ii.permanent = true;
                    out.items[p] = ii;
                    continue;
                }
            }
        }
        // 玩家出生点：清掉 '@'，记录 start
        for (int y = 0; y < out.h; y++)
            for (int x = 0; x < out.w; x++)
                if (out.grid[y][x] == '@') {
                    out.start = {x, y};
                    out.grid[y][x] = '.';
                    out.origGrid[y][x] = '.';
                    break;
                }
        if (out.start.x < 0) out.start = {1, 1};
        return true;
    }

    // 轮回重置：怪物、道具、门恢复初始；保留探索/标记/永久道具
    void resetLoop() {
        grid = origGrid;
        for (auto& kv : monsters) kv.second.alive = true;
        for (auto& kv : items) {
            if (!kv.second.permanent) kv.second.taken = false;
        }
        // 永久道具保持 taken；其格子保持已拾取状态（不重新渲染道具）
    }

    // 序列化（供存档）
    void save(std::ostream& os) const {
        os.write((const char*)&w, sizeof w);
        os.write((const char*)&h, sizeof h);
        os.write((const char*)&start, sizeof start);
        for (const auto& r : grid) { int n = (int)r.size(); os.write((const char*)&n, sizeof n); os.write(r.data(), n); }
        for (const auto& r : origGrid) { int n = (int)r.size(); os.write((const char*)&n, sizeof n); os.write(r.data(), n); }
        // 字符集合
        auto saveChars = [&](const std::set<char>& s) {
            int n = (int)s.size();
            os.write((const char*)&n, sizeof n);
            for (char c : s) os.write(&c, 1);
        };
        saveChars(monsterChars);
        saveChars(itemChars);
        // 怪物
        int mn = (int)monsters.size();
        os.write((const char*)&mn, sizeof mn);
        for (auto& kv : monsters) {
            const Point& p = kv.first;
            os.write((const char*)&p.x, sizeof p.x);
            os.write((const char*)&p.y, sizeof p.y);
            int idn = (int)kv.second.id.size();
            os.write((const char*)&idn, sizeof idn);
            os.write(kv.second.id.data(), idn);
            os.write((const char*)&kv.second.alive, sizeof kv.second.alive);
            os.write((const char*)&kv.second.ch, sizeof kv.second.ch);
        }
        // 道具
        int in = (int)items.size();
        os.write((const char*)&in, sizeof in);
        for (auto& kv : items) {
            const Point& p = kv.first;
            os.write((const char*)&p.x, sizeof p.x);
            os.write((const char*)&p.y, sizeof p.y);
            int idn = (int)kv.second.id.size();
            os.write((const char*)&idn, sizeof idn);
            os.write(kv.second.id.data(), idn);
            os.write((const char*)&kv.second.taken, sizeof kv.second.taken);
            os.write((const char*)&kv.second.permanent, sizeof kv.second.permanent);
        }
        // 探索
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                bool b = isExplored(x, y);
                os.write((const char*)&b, sizeof b);
            }
        }
        // 标记墙
        int mwn = (int)markedWalls.size();
        os.write((const char*)&mwn, sizeof mwn);
        for (auto& kv : markedWalls) {
            os.write((const char*)&kv.first.x, sizeof kv.first.x);
            os.write((const char*)&kv.first.y, sizeof kv.first.y);
        }
    }
    void load(std::istream& is) {
        is.read((char*)&w, sizeof w);
        is.read((char*)&h, sizeof h);
        is.read((char*)&start, sizeof start);
        grid.clear(); origGrid.clear();
        for (int i = 0; i < 2; i++) {
            auto& target = (i == 0) ? grid : origGrid;
            for (int y = 0; y < h; y++) {
                int n = 0; is.read((char*)&n, sizeof n);
                std::string r; r.resize(n);
                if (n) is.read(&r[0], n);
                target.push_back(r);
            }
        }
        int mn = 0; is.read((char*)&mn, sizeof mn);
        monsterChars.clear();
        for (int i = 0; i < mn; i++) { char c; is.read(&c, 1); monsterChars.insert(c); }
        mn = 0; is.read((char*)&mn, sizeof mn);
        itemChars.clear();
        for (int i = 0; i < mn; i++) { char c; is.read(&c, 1); itemChars.insert(c); }
        mn = 0; is.read((char*)&mn, sizeof mn);
        monsters.clear();
        for (int i = 0; i < mn; i++) {
            Point p; is.read((char*)&p.x, sizeof p.x); is.read((char*)&p.y, sizeof p.y);
            int idn = 0; is.read((char*)&idn, sizeof idn);
            std::string id; id.resize(idn);
            if (idn) is.read(&id[0], idn);
            bool alive = false; is.read((char*)&alive, sizeof alive);
            char ch = 'a'; is.read((char*)&ch, sizeof ch);
            monsters[p] = MonsterInst{id, alive, ch};
        }
        int in = 0; is.read((char*)&in, sizeof in);
        items.clear();
        for (int i = 0; i < in; i++) {
            Point p; is.read((char*)&p.x, sizeof p.x); is.read((char*)&p.y, sizeof p.y);
            int idn = 0; is.read((char*)&idn, sizeof idn);
            std::string id; id.resize(idn);
            if (idn) is.read(&id[0], idn);
            bool taken = false, perm = false;
            is.read((char*)&taken, sizeof taken);
            is.read((char*)&perm, sizeof perm);
            items[p] = ItemInst{id, taken, perm};
        }
        explored.assign(h, std::vector<bool>(w, false));
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                bool b = false; is.read((char*)&b, sizeof b);
                if (b) explored[y][x] = true;
            }
        markedWalls.clear();
        int mwn = 0; is.read((char*)&mwn, sizeof mwn);
        for (int i = 0; i < mwn; i++) {
            Point p; is.read((char*)&p.x, sizeof p.x); is.read((char*)&p.y, sizeof p.y);
            markedWalls[p] = true;
        }
        // 恢复楼梯位置
        downStair = {-1, -1}; upStair = {-1, -1};
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                char c = grid[y][x];
                if (c == '>') upStair = {x, y};
                else if (c == '<') downStair = {x, y};
            }
    }
};