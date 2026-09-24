// events.h —— 剧情引擎：条件求值 + 对话场景数据
// 触发条件示例: {"loop>=4", "flag_met_ling", "!killed(any)", "frag>=7"}
#pragma once
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include "json.h"

// ---------- 条件求值上下文 ----------
struct CondContext {
    int loop = 0;          // 轮回次数
    int fragments = 0;     // 记忆碎片
    int obsession = 0;     // 执念
    int gold = 0;
    int exp = 0;
    int atk = 0;
    int def = 0;
    int floor = 1;         // 当前楼层编号
    const std::vector<std::string>* flags = nullptr;
    const std::map<std::string, int>* kills = nullptr; // 怪物id -> 击杀数

    bool hasFlag(const std::string& f) const {
        if (!flags) return false;
        return std::find(flags->begin(), flags->end(), f) != flags->end();
    }
    int killsOf(const std::string& id) const {
        if (!kills) return 0;
        auto it = kills->find(id);
        return it == kills->end() ? 0 : it->second;
    }
    int killsOfTotal() const {
        if (!kills) return 0;
        int total = 0;
        for (auto& kv : *kills) total += kv.second;
        return total;
    }
};

// 具体求值（支持单条件 + && / || 简单组合）
inline bool evalCondInner(const std::string& raw, const CondContext& ctx) {
    std::string c = raw;
    // 去除首尾空白
    while (!c.empty() && (c.front() == ' ' || c.front() == '\t')) c.erase(c.begin());
    while (!c.empty() && (c.back() == ' ' || c.back() == '\t')) c.pop_back();
    if (c.empty()) return true;
    // 先处理 || 和 &&，按最简单的方式从左到右
    size_t orPos = c.find("||");
    if (orPos != std::string::npos) {
        return evalCondInner(c.substr(0, orPos), ctx) || evalCondInner(c.substr(orPos + 2), ctx);
    }
    size_t andPos = c.find("&&");
    if (andPos != std::string::npos) {
        return evalCondInner(c.substr(0, andPos), ctx) && evalCondInner(c.substr(andPos + 2), ctx);
    }
    // 单纯的 flag / !flag / 数值比较 / killed()
    bool negate = false;
    if (!c.empty() && c[0] == '!') { negate = true; c = c.substr(1); }
    if (c.rfind("flag_", 0) == 0) { bool v = ctx.hasFlag(c.substr(5)); return negate ? !v : v; }
    if (c.rfind("seen_", 0) == 0) { bool v = ctx.hasFlag("seen_" + c.substr(5)); return negate ? !v : v; }
    if (c.rfind("killed(", 0) == 0 && c.back() == ')') {
        std::string id = c.substr(7, c.size() - 8);
        bool v = (id == "any") ? (ctx.killsOfTotal() > 0) : (ctx.killsOf(id) > 0);
        return negate ? !v : v;
    }
    // 数值比较
    static const char* ops[] = { ">=", "<=", "==", ">", "<" };
    for (auto op : ops) {
        size_t p = c.find(op);
        if (p != std::string::npos) {
            std::string lhs = c.substr(0, p);
            std::string rhs = c.substr(p + std::strlen(op));
            int r = 0;
            try { r = std::stoi(rhs); } catch (...) { return negate; }
            int a = 0;
            if (lhs == "loop") a = ctx.loop;
            else if (lhs == "frag") a = ctx.fragments;
            else if (lhs == "obs") a = ctx.obsession;
            else if (lhs == "gold") a = ctx.gold;
            else if (lhs == "exp") a = ctx.exp;
            else if (lhs == "atk") a = ctx.atk;
            else if (lhs == "def") a = ctx.def;
            else if (lhs == "floor") a = ctx.floor;
            else return negate;
            bool v = false;
            if (std::string(op) == ">=") v = a >= r;
            else if (std::string(op) == "<=") v = a <= r;
            else if (std::string(op) == "==") v = a == r;
            else if (std::string(op) == ">") v = a > r;
            else if (std::string(op) == "<") v = a < r;
            return negate ? !v : v;
        }
    }
    return negate;
}

// 解析单个条件
inline bool evalCond(const std::string& cond, const CondContext& ctx) {
    std::string c = cond;
    // 去掉空白
    while (!c.empty() && (c.front() == ' ' || c.front() == '\t')) c.erase(c.begin());
    while (!c.empty() && (c.back() == ' ' || c.back() == '\t')) c.pop_back();
    if (c.empty()) return true;
    return evalCondInner(c, ctx);
}

// ---------- 对话场景 ----------
struct DialogChoice {
    std::string text;
    std::string require;  // 选择项可见条件
    std::string set_flag; // 选择后设置旗标
    std::string goto_n;   // 跳转节点 id（"end" 表示结束）
};
struct DialogNode {
    std::string id;       // 节点名（隐式 node_i）
    std::string who;
    std::string text;
    std::string when;     // 节点条件（不满足则跳过）
    std::string set_flag;
    std::string goto_n;
    std::vector<DialogChoice> choices;
};
struct DialogScene {
    std::string id;
    std::vector<std::string> triggers; // 全部满足才触发
    std::vector<DialogNode> nodes;
    bool once = true;                  // 触发后自动标记 seen_<id>
    bool cinema = false;               // 影院模式：黑暗间隔自动播放
};

inline bool loadEvents(const std::string& path, std::map<std::string, DialogScene>& out, std::string& err) {
    jx::Json root;
    if (!jx::parseFile(path, root, err)) return false;
    if (!root.isObj()) { err = "events.json 顶层应为对象"; return false; }
    for (auto& kv : root.obj) {
        DialogScene s;
        s.id = kv.first;
        const jx::Json& e = kv.second;
        s.once = e.boolVal("once", true);
        s.cinema = e.boolVal("cinema", false);
        if (e.get("trigger") && e.get("trigger")->isArr())
            for (auto& t : e.get("trigger")->arr) s.triggers.push_back(t.asString());
        if (e.get("dialog") && e.get("dialog")->isArr()) {
            int idx = 0;
            for (auto& n : e.get("dialog")->arr) {
                DialogNode node;
                node.id = n.strVal("id", "node_" + std::to_string(idx++));
                node.who = n.strVal("who");
                node.text = n.strVal("text");
                node.when = n.strVal("when");
                node.set_flag = n.strVal("set_flag");
                node.goto_n = n.strVal("goto");
                if (n.get("choices") && n.get("choices")->isArr()) {
                    for (auto& ch : n.get("choices")->arr) {
                        DialogChoice dch;
                        dch.text = ch.strVal("text");
                        dch.require = ch.strVal("require");
                        dch.set_flag = ch.strVal("set_flag");
                        dch.goto_n = ch.strVal("goto");
                        node.choices.push_back(std::move(dch));
                    }
                }
                s.nodes.push_back(std::move(node));
            }
        }
        out[s.id] = std::move(s);
    }
    return true;
}

// 填充 {占位符}
inline std::string fillPlaceholders(std::string s, const CondContext& ctx) {
    auto rep = [&](const std::string& key, const std::string& val) {
        size_t p = 0;
        while ((p = s.find("{" + key + "}", p)) != std::string::npos) {
            s.replace(p, key.size() + 2, val);
            p += val.size();
        }
    };
    rep("loop", std::to_string(ctx.loop));
    rep("frag", std::to_string(ctx.fragments));
    rep("obs", std::to_string(ctx.obsession));
    rep("gold", std::to_string(ctx.gold));
    rep("exp", std::to_string(ctx.exp));
    rep("atk", std::to_string(ctx.atk));
    rep("def", std::to_string(ctx.def));
    rep("floor", std::to_string(ctx.floor));
    return s;
}

// 场景触发判定
inline bool scenePasses(const DialogScene& s, const CondContext& ctx) {
    if (s.triggers.empty()) return true;
    for (auto& t : s.triggers)
        if (!evalCondInner(t, ctx)) return false;
    return true;
}