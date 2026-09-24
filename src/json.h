// json.h —— 极简 JSON 解析器（对象/数组/字符串/数字/布尔/null）
// 供数据驱动加载：monsters.json / items.json / floors.json / events.json / endings.json
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace jx {

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj };
    Type t = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj; // 保留键序

    bool isNull() const { return t == Null; }
    bool isArr()  const { return t == Arr; }
    bool isObj()  const { return t == Obj; }
    bool isBool() const { return t == Bool; }
    bool isStr()  const { return t == Str; }

    // 对象访问
    const Json* get(const std::string& k) const {
        for (auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    bool has(const std::string& k) const { return get(k) != nullptr; }
    // 带默认值
    std::string  strVal(const std::string& k, const std::string& d = "") const {
        const Json* p = get(k);
        return (p && p->t == Str) ? p->str : d;
    }
    double numVal(const std::string& k, double d = 0) const {
        const Json* p = get(k);
        return (p && p->t == Num) ? p->num : d;
    }
    int intVal(const std::string& k, int d = 0) const {
        const Json* p = get(k);
        return (p && p->t == Num) ? (int)p->num : d;
    }
    bool boolVal(const std::string& k, bool d = false) const {
        const Json* p = get(k);
        return (p && p->t == Bool) ? p->b : d;
    }

    // 便捷转换
    std::string asString() const { return (t == Str) ? str : ""; }
    int asInt() const { return (t == Num) ? (int)num : 0; }
    double asDouble() const { return (t == Num) ? num : 0; }
};

// ---------- 解析核心 ----------
namespace detail {
inline void skipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
}
inline bool parseString(const std::string& s, size_t& i, std::string& out, std::string& err) {
    if (i >= s.size() || s[i] != '"') { err = "期望字符串"; return false; }
    i++;
    out.clear();
    while (i < s.size()) {
        char c = s[i];
        if (c == '"') { i++; return true; }
        if (c == '\\') {
            i++;
            if (i >= s.size()) break;
            char e = s[i];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'u': {
                    // 只处理 BMP 简单转义（\uXXXX）
                    if (i + 4 < s.size()) {
                        unsigned cp = 0;
                        bool ok = true;
                        for (int k = 1; k <= 4; k++) {
                            char h = s[i + k];
                            int v = -1;
                            if (h >= '0' && h <= '9') v = h - '0';
                            else if (h >= 'a' && h <= 'f') v = h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') v = h - 'A' + 10;
                            if (v < 0) { ok = false; break; }
                            cp = cp * 16 + (unsigned)v;
                        }
                        if (ok) {
                            i += 4;
                            // 编码为 UTF-8
                            if (cp < 0x80) out += (char)cp;
                            else if (cp < 0x800) {
                                out += (char)(0xC0 | (cp >> 6));
                                out += (char)(0x80 | (cp & 0x3F));
                            } else {
                                out += (char)(0xE0 | (cp >> 12));
                                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                                out += (char)(0x80 | (cp & 0x3F));
                            }
                        } else { err = "非法\\u转义"; return false; }
                    } else { err = "截断的\\u转义"; return false; }
                    break;
                }
                default: out += e; break;
            }
            i++;
        } else {
            out += c;
            i++;
        }
    }
    err = "字符串未闭合";
    return false;
}

inline bool parseValue(const std::string& s, size_t& i, Json& out, std::string& err);

inline bool parseArray(const std::string& s, size_t& i, Json& out, std::string& err) {
    i++; // '['
    out.t = Json::Arr;
    while (true) {
        skipWs(s, i);
        if (i >= s.size()) { err = "数组未闭合"; return false; }
        if (s[i] == ']') { i++; return true; }
        Json item;
        if (!parseValue(s, i, item, err)) return false;
        out.arr.push_back(std::move(item));
        skipWs(s, i);
        if (i >= s.size()) { err = "数组未闭合"; return false; }
        if (s[i] == ',') { i++; continue; }
        if (s[i] == ']') { i++; return true; }
        err = "数组内期望 ',' 或 ']'";
        return false;
    }
}

inline bool parseObject(const std::string& s, size_t& i, Json& out, std::string& err) {
    i++; // '{'
    out.t = Json::Obj;
    while (true) {
        skipWs(s, i);
        if (i >= s.size()) { err = "对象未闭合"; return false; }
        if (s[i] == '}') { i++; return true; }
        std::string key;
        if (!parseString(s, i, key, err)) return false;
        skipWs(s, i);
        if (i >= s.size() || s[i] != ':') { err = "期望 ':'"; return false; }
        i++;
        skipWs(s, i);
        Json val;
        if (!parseValue(s, i, val, err)) return false;
        out.obj.emplace_back(std::move(key), std::move(val));
        skipWs(s, i);
        if (i >= s.size()) { err = "对象未闭合"; return false; }
        if (s[i] == ',') { i++; continue; }
        if (s[i] == '}') { i++; return true; }
        err = "对象内期望 ',' 或 '}'";
        return false;
    }
}

inline bool parseValue(const std::string& s, size_t& i, Json& out, std::string& err) {
    skipWs(s, i);
    if (i >= s.size()) { err = "意外结尾"; return false; }
    char c = s[i];
    if (c == '{') return parseObject(s, i, out, err);
    if (c == '[') return parseArray(s, i, out, err);
    if (c == '"') {
        std::string tmp;
        if (!parseString(s, i, tmp, err)) return false;
        out.str = std::move(tmp);
        out.t = Json::Str;
        return true;
    }
    if (c == 't') { if (s.compare(i, 4, "true") == 0) { i += 4; out.t = Json::Bool; out.b = true; return true; } }
    if (c == 'f') { if (s.compare(i, 5, "false") == 0) { i += 5; out.t = Json::Bool; out.b = false; return true; } }
    if (c == 'n') { if (s.compare(i, 4, "null") == 0) { i += 4; out.t = Json::Null; return true; } }
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = i;
        if (c == '-') i++;
        while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) i++;
        out.t = Json::Num;
        out.num = std::strtod(s.substr(start, i - start).c_str(), nullptr);
        return true;
    }
    err = std::string("无法解析字符: ") + c;
    return false;
}
} // namespace detail

// 解析字符串 -> Json
inline bool parse(const std::string& text, Json& out, std::string& err) {
    size_t i = 0;
    if (!detail::parseValue(text, i, out, err)) return false;
    detail::skipWs(text, i);
    if (i != text.size()) { err = "尾部存在多余内容"; return false; }
    return true;
}

// 读取文件并解析
inline bool parseFile(const std::string& path, Json& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "无法打开文件: " + path; return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    // 去除 BOM
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text = text.substr(3);
    return parse(text, out, err);
}

} // namespace jx