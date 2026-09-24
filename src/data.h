// data.h —— 数据定义与加载（全部来自 data/ 下的 JSON）
#pragma once
#include <map>
#include <string>
#include <vector>
#include "json.h"
#include "types.h"

// ---------- 怪物 ----------
struct MonsterDef {
    std::string id, name;
    int hp = 0, atk = 0, def = 0, gold = 0, exp = 0;
    std::vector<std::string> skills;   // 技能字符串
    int curseAtk = 5;                  // 诅咒战后扣攻
    std::string splitInto;             // 分裂产物 id
    std::string soulLine;              // 轮回10+台词彩蛋
    bool spare = false;                // 轮回10+让路
    bool boss = false;
    std::string desc;
};
inline bool loadMonsters(const std::string& path, std::map<std::string, MonsterDef>& out, std::string& err) {
    jx::Json root;
    if (!jx::parseFile(path, root, err)) return false;
    if (!root.isArr()) { err = "monsters.json 顶层应为数组"; return false; }
    for (auto& m : root.arr) {
        MonsterDef d;
        d.id       = m.strVal("id");
        d.name     = m.strVal("name", d.id);
        d.hp       = m.intVal("hp");
        d.atk      = m.intVal("atk");
        d.def      = m.intVal("def");
        d.gold     = m.intVal("gold");
        d.exp      = m.intVal("exp");
        d.curseAtk = m.intVal("curse_atk", 5);
        d.splitInto= m.strVal("split_into");
        d.soulLine = m.strVal("soul_line");
        d.spare    = m.boolVal("spare");
        d.boss     = m.boolVal("boss");
        d.desc     = m.strVal("desc");
        if (m.get("skills") && m.get("skills")->isArr())
            for (auto& s : m.get("skills")->arr) d.skills.push_back(s.asString());
        out[d.id] = std::move(d);
    }
    return true;
}

// ---------- 道具 ----------
struct ItemDef {
    std::string id, name, type;   // type: hp | atk | def | gold | fragment | relic | misc
    int value = 0;
    std::string relicEffect;      // 遗物效果键
    std::string desc;
    std::string pickupText;
};
inline bool loadItems(const std::string& path, std::map<std::string, ItemDef>& out, std::string& err) {
    jx::Json root;
    if (!jx::parseFile(path, root, err)) return false;
    if (!root.isObj()) { err = "items.json 顶层应为对象"; return false; }
    for (auto& kv : root.obj) {
        ItemDef d;
        const jx::Json& m = kv.second;
        d.id    = kv.first;
        d.name  = m.strVal("name", d.id);
        d.type  = m.strVal("type", "misc");
        d.value = m.intVal("value");
        d.relicEffect = m.strVal("relic_effect");
        d.desc  = m.strVal("desc");
        d.pickupText = m.strVal("pickup_text");
        out[d.id] = std::move(d);
    }
    return true;
}

// ---------- 楼层 ----------
struct FloorConfig {
    std::string id, file, name, goal;
    Point start;                       // 玩家出生点（地图中的 @）
    std::map<char, std::string> monsters; // 字符 -> 怪物 id
    std::map<char, std::string> items;    // 字符 -> 道具 id
    std::string npcEvent;              // 'n' 绑定事件
    std::string triggerEvent;          // 'T' 绑定事件
    bool shop = false;                 // 是否有 's' 商店
};
inline bool loadFloors(const std::string& path, const std::string& floorDir,
                       std::map<std::string, FloorConfig>& out, std::string& err) {
    jx::Json root;
    if (!jx::parseFile(path, root, err)) return false;
    if (!root.isArr()) { err = "floors.json 顶层应为数组"; return false; }
    for (auto& f : root.arr) {
        FloorConfig c;
        c.id   = f.strVal("id");
        c.file = floorDir + "\\" + f.strVal("file");
        c.name = f.strVal("name", c.id);
        c.goal = f.strVal("goal");
        c.npcEvent     = f.strVal("npc_event");
        c.triggerEvent = f.strVal("trigger_event");
        c.shop         = f.boolVal("shop");
        c.start        = { f.intVal("start_x"), f.intVal("start_y") };
        if (f.get("monsters") && f.get("monsters")->isObj())
            for (auto& kv : f.get("monsters")->obj)
                if (kv.second.isStr() && !kv.first.empty())
                    c.monsters[kv.first[0]] = kv.second.asString();
        if (f.get("items") && f.get("items")->isObj())
            for (auto& kv : f.get("items")->obj)
                if (kv.second.isStr() && !kv.first.empty())
                    c.items[kv.first[0]] = kv.second.asString();
        out[c.id] = std::move(c);
    }
    return true;
}

// ---------- 结局 ----------
struct EndingDef {
    std::string id, name;
    std::vector<std::string> conds;
    std::vector<std::string> text;
};
inline bool loadEndings(const std::string& path, std::vector<EndingDef>& out, std::string& err) {
    jx::Json root;
    if (!jx::parseFile(path, root, err)) return false;
    if (!root.isArr()) { err = "endings.json 顶层应为数组"; return false; }
    for (auto& e : root.arr) {
        EndingDef d;
        d.id   = e.strVal("id");
        d.name = e.strVal("name");
        if (e.get("conds") && e.get("conds")->isArr())
            for (auto& c : e.get("conds")->arr) d.conds.push_back(c.asString());
        if (e.get("text") && e.get("text")->isArr())
            for (auto& t : e.get("text")->arr) d.text.push_back(t.asString());
        out.push_back(std::move(d));
    }
    return true;
}

// ---------- 全局数据包 ----------
struct GameData {
    std::map<std::string, MonsterDef> monsters;
    std::map<std::string, ItemDef> items;
    std::map<std::string, FloorConfig> floors;
    std::vector<EndingDef> endings;

    bool loadAll(const std::string& dataDir, std::string& err) {
        if (!loadMonsters(dataDir + "\\monsters.json", monsters, err)) return false;
        if (!loadItems(dataDir + "\\items.json", items, err)) return false;
        if (!loadFloors(dataDir + "\\floors.json", dataDir + "\\floors", floors, err)) return false;
        if (!loadEndings(dataDir + "\\endings.json", endings, err)) return false;
        // 校验：怪物分裂产物存在
        for (auto& kv : monsters) {
            if (!kv.second.splitInto.empty() && !monsters.count(kv.second.splitInto)) {
                err = "怪物 " + kv.first + " 的分裂产物不存在: " + kv.second.splitInto;
                return false;
            }
        }
        return true;
    }
    const MonsterDef* monster(const std::string& id) const {
        auto it = monsters.find(id);
        return it == monsters.end() ? nullptr : &it->second;
    }
    const ItemDef* item(const std::string& id) const {
        auto it = items.find(id);
        return it == items.end() ? nullptr : &it->second;
    }
};