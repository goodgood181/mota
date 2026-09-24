// main.cpp —— 入口：主菜单 / 游戏循环 / --selftest 自检
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <set>
#include <vector>
#include "game.h"
#include "battle.h"
#include "events.h"
#include "term.h"

#ifdef _WIN32
#include <windows.h>
#endif

static std::string exeDir() {
#ifdef _WIN32
    char buf[1024];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    std::string p(buf, n);
    size_t pos = p.find_last_of("\\/");
    if (pos != std::string::npos) p = p.substr(0, pos);
    return p;
#else
    return ".";
#endif
}

// 查找 data 目录：exe旁 -> 当前目录 -> 父目录
static std::string findDataDir() {
    auto exists = [](const std::string& p) {
        std::ifstream f(p + "\\monsters.json");
        return (bool)f;
    };
    std::string cand[3] = {
        exeDir() + "\\data",
        ".\\data",
        exeDir() + "\\..\\data"
    };
    for (auto& c : cand)
        if (exists(c)) return c;
    return cand[0];
}

// ---------------- 游戏循环 ----------------
static void play(Game* g) {
    g->running = true;
    while (g->running) {
        if (ui::active() && ui::quitRequested()) { g->running = false; break; }
        g->render();
        g->handleKey(term::readKey());
    }
}

// 解析脚本按键：UP/DOWN/LEFT/RIGHT/ENT/SPC/ESC 或任意单字符
static void loadScript(const std::string& path) {
    std::ifstream f(path);
    if (!f) { printf("无法打开脚本: %s\n", path.c_str()); return; }
    std::string tok;
    while (f >> tok) {
        int k = 0;
        if (tok == "UP") k = 0x11;
        else if (tok == "DOWN") k = 0x12;
        else if (tok == "LEFT") k = 0x13;
        else if (tok == "RIGHT") k = 0x14;
        else if (tok == "ENT" || tok == "ENTER") k = 0x0D;
        else if (tok == "SPC") k = ' ';
        else if (tok == "ESC") k = 0x1B;
        else k = tok[0];
        if (k) term::g_scriptKeys.push_back(k);
    }
    printf("脚本已载入: %zu 个按键\n", term::g_scriptKeys.size());
}

// ---------------- 自检 ----------------
static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  [ok] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static int runSelftest(const std::string& dataDir) {
    setvbuf(stdout, nullptr, _IONBF, 0); // 无缓冲输出，便于定位卡点
    printf("== 《轮回之塔》自检 ==\n");

    // 1. 数据加载
    Game g0;
    std::string err;
    if (!g0.init(dataDir, err)) {
        printf("数据加载失败: %s\n", err.c_str());
        return 1;
    }
    CHECK(true, "数据加载 (monsters/items/floors/endings/events)");
    CHECK(g0.data.monsters.size() >= 17, "怪物数量充足");
    CHECK(g0.data.items.size() >= 17, "道具数量充足");
    CHECK(g0.floorOrder.size() >= 20, "楼层数量充足 (20 层 / 3 章)");

    // 2. 楼层完整性
    g0.newGame();
    bool mapOk = true;
    std::set<int> carriedKeys; // 跨层携带的钥匙（3F 红钥匙 → 4F 红门）
    for (size_t i = 0; i < g0.floorOrder.size(); i++) {
        const std::string& id = g0.floorOrder[i];
        auto& fl = g0.floors[id];
        if (fl.h <= 0 || fl.w <= 0) { printf("  ! 楼层 %s 尺寸非法\n", id.c_str()); mapOk = false; }
        if (fl.start.x < 0) { printf("  ! 楼层 %s 无出生点\n", id.c_str()); mapOk = false; }
        // 行宽一致
        for (auto& row : fl.grid)
            if ((int)row.size() != fl.w) { printf("  ! 楼层 %s 行宽不一致\n", id.c_str()); mapOk = false; }
        // 实体 id 可解析
        for (auto& kv : fl.monsters)
            if (!g0.data.monster(kv.second.id)) { printf("  ! 楼层 %s 怪物id缺失: %s\n", id.c_str(), kv.second.id.c_str()); mapOk = false; }
        for (auto& kv : fl.items)
            if (!g0.data.item(kv.second.id)) { printf("  ! 楼层 %s 道具id缺失: %s\n", id.c_str(), kv.second.id.c_str()); mapOk = false; }
        // 楼梯预期：1F 有上楼梯；2-4F 上下都有；5F 有下楼梯
        bool hasUp = fl.upStair.x >= 0, hasDown = fl.downStair.x >= 0;
        if (i == 0 && !hasUp) { printf("  ! 楼层 %s 缺少上楼梯\n", id.c_str()); mapOk = false; }
        if (i > 0 && i + 1 < g0.floorOrder.size() && (!hasUp || !hasDown)) {
            printf("  ! 楼层 %s 缺少楼梯\n", id.c_str()); mapOk = false;
        }
        if (i + 1 == g0.floorOrder.size() && !hasDown) {
            printf("  ! 楼层 %s 缺少下楼梯\n", id.c_str()); mapOk = false;
        }
        // 主线目标（goal）必须存在
        if (fl.goal.empty()) {
            printf("  ! 楼层 %s 缺少主线目标(goal)\n", id.c_str());
            mapOk = false;
        }
        // 游戏机制：门必经 / 钥匙门前可达 / 开门后全连通（怪物视为可通行）
        if (fl.h > 0 && fl.w > 0 && fl.start.x >= 0 && fl.start.y >= 0 &&
            fl.start.x < fl.w && fl.start.y < fl.h) {
            auto bfsReach = [&](bool doorsPass) -> std::vector<std::vector<bool>> {
                std::vector<std::vector<bool>> vis(fl.h, std::vector<bool>(fl.w, false));
                std::vector<Point> q{ fl.start };
                vis[fl.start.y][fl.start.x] = true;
                while (!q.empty()) {
                    Point p = q.back(); q.pop_back();
                    for (int d = 0; d < 4; d++) {
                        Point np = p + dirDelta(d);
                        if (!fl.inBounds(np.x, np.y) || vis[np.y][np.x]) continue;
                        if (fl.grid[np.y][np.x] == '#') continue;
                        if (!doorsPass && fl.isDoor(np.x, np.y)) continue;
                        vis[np.y][np.x] = true;
                        q.push_back(np);
                    }
                }
                return vis;
            };
            // (a) 开门后全层连通（防孤岛）
            {
                auto vis = bfsReach(true);
                int reach = 0, total = 0;
                for (int y = 0; y < fl.h; y++)
                    for (int x = 0; x < fl.w; x++)
                        if (fl.grid[y][x] != '#') { total++; if (vis[y][x]) reach++; }
                if (reach * 100 < total * 95) {
                    printf("  ! 楼层 %s 开门后连通性差: 可达 %d/%d\n", id.c_str(), reach, total);
                    mapOk = false;
                }
            }
            // (b) 每扇门必须是"门前 ↔ 门后"的唯一桥（门必经，不可绕过）。
            //     检查时仅把【这一扇门】当墙，其余门可通行（适配 9F 双门串联）。
            for (int y = 0; y < fl.h; y++)
                for (int x = 0; x < fl.w; x++) {
                    if (!fl.isDoor(x, y)) continue;
                    char saved = fl.grid[y][x];
                    fl.grid[y][x] = '#';
                    auto vis = bfsReach(true);
                    fl.grid[y][x] = saved;
                    bool anyReach = false, anyUnreach = false;
                    for (int d = 0; d < 4; d++) {
                        Point np = Point{x, y} + dirDelta(d);
                        if (!fl.inBounds(np.x, np.y) || fl.grid[np.y][np.x] == '#') continue;
                        if (vis[np.y][np.x]) anyReach = true; else anyUnreach = true;
                    }
                    if (!anyReach || !anyUnreach) {
                        printf("  ! 楼层 %s 门 (%d,%d) 可绕过或无效\n", id.c_str(), x, y);
                        mapOk = false;
                    }
                }
            // (c) 钥匙→门 开锁链：按可达性逐扇开门（模拟真实游玩顺序）。
            //     最终要求：所有钥匙都能拾取，所有门都能被其钥匙打开并穿过（不会卡死）。
            {
                auto bfsOpen = [&](const std::set<Point>& open) {
                    std::vector<std::vector<bool>> vis2(fl.h, std::vector<bool>(fl.w, false));
                    std::vector<Point> q{ fl.start };
                    vis2[fl.start.y][fl.start.x] = true;
                    while (!q.empty()) {
                        Point p = q.back(); q.pop_back();
                        for (int d = 0; d < 4; d++) {
                            Point np = p + dirDelta(d);
                            if (!fl.inBounds(np.x, np.y) || vis2[np.y][np.x]) continue;
                            if (fl.grid[np.y][np.x] == '#') continue;
                            if (fl.isDoor(np.x, np.y) && !open.count(np)) continue;
                            vis2[np.y][np.x] = true;
                            q.push_back(np);
                        }
                    }
                    return vis2;
                };
                std::set<Point> open;
                auto vis = bfsOpen(open);
                bool more = true;
                int guard = 0;
                while (more && guard++ < 64) {
                    more = false;
                    for (int y = 0; y < fl.h; y++)
                        for (int x = 0; x < fl.w; x++) {
                            if (!fl.isDoor(x, y) || open.count({x, y})) continue;
                            int k = fl.doorColor(x, y);
                            bool haveKey = carriedKeys.count(k) > 0;
                            for (auto& kv : fl.items) {
                                const ItemDef* it = g0.data.item(kv.second.id);
                                if (!it || it->type != "key" || it->value != k) continue;
                                if (vis[kv.first.y][kv.first.x]) { haveKey = true; break; }
                            }
                            if (!haveKey) continue;
                            open.insert({x, y});
                            auto nv = bfsOpen(open);
                            bool grew = false;
                            for (int yy = 0; yy < fl.h; yy++)
                                for (int xx = 0; xx < fl.w; xx++)
                                    if (nv[yy][xx] && !vis[yy][xx]) { grew = true; break; }
                            if (grew) { vis = nv; more = true; }
                            else open.erase({x, y});
                        }
                }
                for (auto& kv : fl.items) {
                    const ItemDef* it = g0.data.item(kv.second.id);
                    if (!it || it->type != "key") continue;
                    if (!vis[kv.first.y][kv.first.x]) {
                        printf("  ! 楼层 %s 钥匙 %s 按开门顺序仍不可达 (%d,%d)\n",
                               id.c_str(), it->name.c_str(), kv.first.x, kv.first.y);
                        mapOk = false;
                    }
                }
                for (int y = 0; y < fl.h; y++)
                    for (int x = 0; x < fl.w; x++)
                        if (fl.isDoor(x, y) && !open.count({x, y})) {
                            printf("  ! 楼层 %s 门 (%d,%d) 无法按开锁顺序打开\n", id.c_str(), x, y);
                            mapOk = false;
                        }
                // 本层结算：可达区内所有钥匙颜色加入携带集合（供后续楼层开门）
                for (auto& kv : fl.items) {
                    const ItemDef* it = g0.data.item(kv.second.id);
                    if (!it || it->type != "key") continue;
                    if (vis[kv.first.y][kv.first.x]) carriedKeys.insert(it->value);
                }
            }
        }
    }
    CHECK(mapOk, "所有楼层地图合法 (尺寸/楼梯/实体/连通)");

    // 3. 战斗公式
    {
        auto r1 = BattleSimulator::simulate(300, 20, 10, 50, 20, 1, {});
        CHECK(r1.heroWins && r1.heroLossHp == 20 && r1.rounds == 3,
              "绿史莱姆: 损血20 / 3回合");
        auto r2 = BattleSimulator::simulate(300, 20, 10, 100, 35, 5, { "combo" });
        CHECK(!r2.heroWins && r2.heroLossHp == 300,
              "连击蝙蝠: 开局300血必死");
        auto r3 = BattleSimulator::simulate(300, 20, 10, 150, 45, 10, {});
        CHECK(!r3.heroWins, "骷髅兵: 开局必死 (红色警告)");
        auto r4 = BattleSimulator::simulate(300, 60, 30, 200, 50, 30, {});
        CHECK(r4.heroWins && r4.heroLossHp > 0, "60攻30防: 可胜");
        auto r5 = BattleSimulator::simulate(300, 60, 30, 200, 60, 50, { "first_strike" });
        CHECK(r5.bossFirstStrike && !r5.heroWins && r5.heroLossHp == 300,
              "先攻: 开战先吃一下（300血恰好阵亡）");
        auto r6 = BattleSimulator::simulate(300, 60, 30, 200, 60, 50, { "first_strike" }, 1.0, 1.0, true);
        CHECK(!r6.bossFirstStrike, "玩家先手抵消怪物先攻");
        // 第 3 章 Boss 心魔·妄：成长后可胜（突破保底数值）
        {
            const MonsterDef* hm = g0.data.monster("heart_demon");
            if (hm) {
                auto h1 = BattleSimulator::simulate(600, 130, 120, hm->hp, hm->atk, hm->def, hm->skills);
                CHECK(!h1.heroWins, "心魔·妄: 初入第 3 章必死");
                auto h2 = BattleSimulator::simulate(1800, 210, 200, hm->hp, hm->atk, hm->def, hm->skills, 1.0, 1.0, true);
                CHECK(h2.heroWins, "心魔·妄: 成长后（攻210/防200）可胜");
            } else {
                CHECK(false, "心魔·妄: 数据缺失");
            }
        }
        auto r7 = BattleSimulator::simulate(300, 100, 90, 200, 60, 50, { "curse" });
        CHECK(r7.curseAtkLoss == 5, "诅咒: 胜利后 -5 攻");
        auto r8 = BattleSimulator::simulate(300, 100, 90, 200, 60, 50, {}, 1.3, 1.3);
        CHECK(r8.heroDmgPerRound == 35 && r8.monDmgPerRound == 0,
              "领域: 怪物攻防+30% (防御65→伤害35/回合)");
        auto r9 = BattleSimulator::simulate(300, 100, 90, 200, 60, 50, { "lifesteal" });
        CHECK(r9.heroWins, "吸血: 正常结算");
        // 赤铠（5F Boss）：开局必死，成长后可胜
        auto rb = BattleSimulator::simulate(300, 20, 10, 1200, 130, 60, { "first_strike" });
        CHECK(!rb.heroWins, "守门人·赤铠: 开局数值必死 (轮回教学)");
        auto rb2 = BattleSimulator::simulate(600, 85, 135, 1200, 130, 60, { "first_strike" });
        CHECK(rb2.heroWins && rb2.heroLossHp == 0, "守门人·赤铠: 成长后可胜 (经验跨轮回的回报)");
        // 镜卫·魁（10F Boss，第 2 章尽头）
        auto rm = BattleSimulator::simulate(800, 110, 160, 1700, 140, 75, { "combo", "first_strike" });
        CHECK(rm.heroWins && rm.heroLossHp == 0, "镜卫·魁: 第 2 章成长后可胜");
    }

    // 3.5 文本自动换行（右侧目标文本框 / 终端目标区）
    {
        auto w1 = term::wrapText("击败守门人·赤铠，登出第 1 章。", 10);
        CHECK(w1.size() >= 2 && (int)term::dispWidth(w1[0]) <= 10 && (int)term::dispWidth(w1[1]) <= 10,
              "目标文本换行: 每行不超宽 (CJK=2列)");
        auto w2 = term::wrapText("【第 2 章 · 镜之回廊】残影在等你。用红钥匙打开红门，穿过石魔的领地。", 22);
        CHECK(w2.size() >= 3, "目标长文本自动换行 (22 列 -> 多行完整)");
        CHECK(term::wrapText("", 10).size() == 1, "换行: 空文本至少一行");
        CHECK(term::wrapText("a,b,c", 1).size() == 5, "换行: 极窄宽度逐字符分行");
        auto w3 = term::wrapText("英文和中文混排 asdfghjkl 自动换行", 10);
        bool noWide = true;
        for (auto& ln : w3) if ((int)term::dispWidth(ln) > 10) noWide = false;
        CHECK(noWide, "换行: 中英混排每行不超宽");
    }

    // 3.6 存档槽位（六格：头部元数据 + 状态流往返）
    {
        Game t;
        std::string e3;
        if (t.init(dataDir, e3)) {
            t.suppressScenes = true;
            t.newGame();
            t.loopCount = 3;
            t.hero.st.hp = 700;
            t.hero.fragments = 2;
            t.obsession = 5;
            std::string oldPrefix = t.savePath;
            t.savePath = dataDir;   // 测试用前缀（写完即删）
            bool ok1 = t.saveGame(0);
            Game::SaveMeta meta;
            bool okm = t.readSaveMeta(0, meta);
            CHECK(ok1 && okm && meta.valid, "存档槽位: 写入 + 元数据有效");
            CHECK(meta.loop == 3 && meta.fragments == 2 && meta.obsession == 5,
                  "存档槽位: 元数据字段正确 (轮回/碎片/执念)");
            CHECK(meta.floorNo == 1 && meta.ts > 0, "存档槽位: 元数据楼层/时间戳");
            Game u;
            if (u.init(dataDir, e3)) {
                u.suppressScenes = true;
                u.savePath = dataDir;
                bool ok2 = u.loadGame(0);
                CHECK(ok2 && u.hero.st.hp == 700 && u.loopCount == 3 && u.hero.fragments == 2,
                      "存档槽位: 读档状态一致");
                CHECK(u.saveSlot == 0, "存档槽位: 载入后绑定槽位 0");
            }
            Game::SaveMeta empty;
            t.readSaveMeta(5, empty);
            CHECK(!empty.valid, "存档槽位: 空槽位 valid=false");
            remove(t.saveName(0).c_str());
            t.savePath = oldPrefix;
        } else {
            CHECK(false, "存档槽位: init 失败");
        }
    }

    // 3.7+ 旧档兼容：RMOTASL7 时代布局（无永久成长字段）仍可载入
    {
        Game a, c;
        std::string e7;
        if (a.init(dataDir, e7) && c.init(dataDir, e7)) {
            a.suppressScenes = true;
            c.suppressScenes = true;
            a.newGame();
            a.permAtk = 30; a.permDef = 15; a.permHp = 900;   // 新字段（旧档中不存在）
            a.hero.st.atk += 50;
            a.hero.st.hp = 640;
            std::stringstream legacy;
            a.saveStreamV(legacy, false);                     // 模拟旧版流（无 perm）
            bool okL = c.loadStreamV(legacy, false);
            CHECK(okL, "旧档兼容: 无 perm 版本流可载入");
            CHECK(c.permAtk == 0 && c.permDef == 0 && c.permHp == 0, "旧档兼容: 旧档永久成长默认 0");
            CHECK(c.hero.st.atk == a.hero.st.atk && c.hero.st.hp == 640,
                  "旧档兼容: 读档属性与保存一致");
        } else {
            CHECK(false, "旧档兼容: init 失败");
        }
    }
    {
        Game t;
        std::string e3;
        if (t.init(dataDir, e3)) {
            t.suppressScenes = true;
            t.newGame();
            CHECK(t.relicEffect().empty(), "遗物: 无遗物时效果键为空");
            t.relicId = "relic_gold";
            CHECK(t.relicEffect() == "start_gold30", "遗物: relic_gold → 效果键 start_gold30");
            t.resetForLoop();
            CHECK(t.hero.st.gold >= 150, "遗物: 启程金币 开局 +150 金");
            t.relicId = "relic_vigor";
            t.resetForLoop();
            CHECK(t.hero.st.hp == 300 + 300, "遗物: 坚韧血瓶 开局 +300 血");
            t.relicId = "relic_chijia";
            CHECK(t.relicEffect() == "first_battle_firststrike", "遗物: 赤铠之怒 → 先手键");
            t.relicId = "relic_bounty";
            CHECK(t.relicEffect() == "double_gold", "遗物: 双倍赏金 → 翻倍键");
            // 永久成长（跨轮回保留；遗物换回血瓶验证叠加）
            t.relicId = "relic_vigor";
            t.permAtk = 25; t.permDef = 25; t.permHp = 1500;
            t.resetForLoop();
            CHECK(t.hero.st.atk == 20 + 25 && t.hero.st.def == 10 + 25,
                  "永久成长: 攻击/防御跨轮回保留");
            CHECK(t.hero.st.hp == 300 + 300 + 1500, "永久成长: 生命 = 基础+遗物+突破 (300+300+1500)");
            t.permDef = 50;
            t.resetForLoop();
            CHECK(t.hero.st.def == 10 + 50, "永久成长: 多次突破后再次轮回仍保留");
        } else {
            CHECK(false, "遗物/永久成长: init 失败");
        }
    }

    // 4. 条件求值
    {
        CondContext c;
        c.loop = 4; c.fragments = 7; c.obsession = 12; c.floor = 3;
        std::vector<std::string> fl{ "met_ling" };
        c.flags = &fl;
        std::map<std::string, int> kills{ { "slime_green", 2 } };
        c.kills = &kills;
        CHECK(evalCondInner("loop>=4", c) && evalCondInner("loop<10", c), "条件: loop 区间");
        CHECK(evalCondInner("flag_met_ling", c) && evalCondInner("!flag_x", c), "条件: flag");
        CHECK(evalCondInner("frag>=7", c) && evalCondInner("obs>=10", c), "条件: frag/obs");
        CHECK(evalCondInner("killed(any)", c) && evalCondInner("!killed(bat_small)", c), "条件: killed");
        CHECK(evalCondInner("loop>=2 && loop<10", c) && evalCondInner("flag_met_ling || loop>=99", c), "条件: 组合运算");
    }

    // 5. 结局条件
    {
        CondContext c;
        c.loop = 12; c.fragments = 7;
        std::vector<std::string> fl; c.flags = &fl;
        std::map<std::string, int> kills{ {"slime_green", 5} }; c.kills = &kills;
        bool bPass = false, aPass = false, dPass = false;
        for (auto& e : g0.data.endings) {
            bool ok = true;
            for (auto& cd : e.conds) if (!evalCondInner(cd, c)) { ok = false; break; }
            if (e.id == "B" && ok) bPass = true;
            if (e.id == "A" && ok) aPass = true;
            if (e.id == "D" && ok) dPass = true;
        }
        CHECK(bPass && aPass && !dPass, "结局条件: B真结局可达 / D需不杀");
    }

    // 6. 存档往返
    {
        Game a, b;
        std::string e1, e2;
        if (!a.init(dataDir, e1) || !b.init(dataDir, e2)) { printf("init失败\n"); return 1; }
        a.newGame();
        a.hero.st.atk += 33;
        a.hero.keys[0] = 2; a.hero.keys[1] = 1;
        a.hero.fragments = 2;
        a.obsession = 17;
        a.loopCount = 3;
        a.relicId = "relic_gold";
        a.hero.setFlag("ask_identity");
        // 杀死 1F 一只史莱姆
        for (auto& kv : a.floors["floor_1"].monsters) { kv.second.alive = false; break; }
        std::stringstream ss;
        a.saveStream(ss);
        bool okL = b.loadStream(ss);
        CHECK(okL, "存档流可载入");
        CHECK(b.hero.st.atk == a.hero.st.atk && b.hero.keys[0] == 2 && b.hero.keys[1] == 1 &&
              b.obsession == 17 && b.loopCount == 3 && b.hero.fragments == 2,
              "存档往返: 属性一致");
        CHECK(b.hero.hasFlag("ask_identity") && b.relicId == "relic_gold", "存档往返: 旗标/遗物");
        bool sameDead = true;
        for (auto& kv : a.floors["floor_1"].monsters)
            if (kv.second.alive != b.floors["floor_1"].monsters[kv.first].alive) sameDead = false;
        CHECK(sameDead, "存档往返: 楼层怪物状态一致");
    }

    // 7. 战斗模拟冒烟（无终端）
    {
        Game g;
        std::string e;
        if (!g.init(dataDir, e)) return 1;
        g.suppressScenes = true;
        g.newGame();
        // 1F 右侧拿黄钥匙（(9,1)）；方向：0上 1下 2左 3右
        for (int i = 0; i < 6; i++) g.tryMove(3); // (3,1)->(9,1) 拾取 k
        CHECK(g.hero.keys[0] == 1, "移动+拾取黄钥匙");
        // 经 x3 通道下 y3 走廊，蛇形绕到 y6/y7 网道
        for (int i = 0; i < 6; i++) g.tryMove(2); // 左回 (3,1)
        g.tryMove(1); g.tryMove(1);               // 下 (3,2) (3,3)
        g.tryMove(3); g.tryMove(3);               // 右 (4,3) (5,3)
        g.tryMove(1);                             // 下 (5,4) 撞开暗墙 H
        g.tryMove(1);                             // 下 (5,5)
        g.tryMove(3); g.tryMove(3);               // 右 (6,5) (7,5)
        g.tryMove(1);                             // 下 (7,6)
        g.tryMove(3); g.tryMove(3);               // 右 (8,6) (9,6)
        g.tryMove(1);                             // 下 (9,7) -> y7 走廊
        bool won7 = g.battleAt({5, 7}, false);    // 清 (5,7) 史莱姆（路径必经）
        CHECK(won7, "清路战: 1F (5,7) 史莱姆");
        for (int i = 0; i < 4; i++) g.tryMove(2); // 左 (8,7)(7,7)(6,7)(5,7)
        g.tryMove(2); g.tryMove(2);               // 左 (4,7)(3,7)
        g.tryMove(0);                             // 上 (3,6)
        g.tryMove(2);                             // 左 (2,6)
        g.tryMove(2);                             // 左 打开必经黄门 (1,6)
        CHECK(g.hero.keys[0] == 0, "开门消耗黄钥匙");
        g.tryMove(1);                             // 下 (1,7) 门后金币（单格死胡同，返身回门）
        g.tryMove(0); g.tryMove(3);               // 上 (1,6) -> 右 (2,6)（门已开）
        g.tryMove(3);                             // 右 (3,6)
        g.tryMove(1);                             // 下 (3,7)
        for (int i = 0; i < 8; i++) g.tryMove(3); // 右 (4..11,7)
        for (int i = 0; i < 6; i++) g.tryMove(0); // 沿 x11 竖道上回 (11,1) 楼梯
        CHECK(g.cur == "floor_2", "踏上楼梯换层至 2F");
        CHECK(g.hero.keys[0] == 0, "上楼后钥匙已用于开门");
    }

    // 8. 实战：战斗 / 商店 / 轮回保留
    {
        Game g;
        std::string e;
        if (!g.init(dataDir, e)) return 1;
        g.suppressScenes = true;
        g.newGame();
        // 战斗：1F (2,3) 的绿史莱姆
        g.player = {1, 3};
        g.facing = 3;
        bool won = g.battleAt({2, 3}, false);
        bool slimeDead = !g.floors["floor_1"].monsters[{2, 3}].alive;
        CHECK(won && g.hero.st.gold == 3 && g.hero.st.hp == 280 &&
              g.codex["slime_green"].kills == 1 && slimeDead,
              "实战: 击杀史莱姆 (损血20/金3/图鉴记录)");
        // 商店：价格递增
        g.hero.st.gold = 100;
        g.shopBuy(Game::SHOP_GOLD_ATK);
        CHECK(g.hero.st.atk == 24 && g.hero.st.gold == 75 && g.shopCount[0] == 1,
              "商店: +4攻 25金");
        g.shopBuy(Game::SHOP_GOLD_ATK);
        CHECK(g.hero.st.gold == 25 && g.hero.st.atk == 28, "商店: 第二次涨价到 50 金");
        // 轮回：属性清空、碎片/执念/图鉴保留、经验跨轮回保留、怪物重生
        g.hero.fragments = 3;
        g.obsession = 20;
        g.loopCount = 1;
        g.hero.st.exp = 55; // 经验是唯一允许带走的「成长」
        g.resetForLoop();
        bool slimeRespawned = g.floors["floor_1"].monsters[{4, 3}].alive;
        CHECK(g.hero.st.hp == 300 && g.hero.st.atk == 20 && g.hero.st.gold == 0 &&
              g.hero.keys[0] == 0, "轮回: 属性与钥匙清空");
        CHECK(g.hero.st.exp == 55, "轮回: 经验跨轮回保留（成长积累）");
        CHECK(g.hero.fragments == 3 && g.obsession == 20 && g.loopCount == 1 &&
              g.codex["slime_green"].kills == 1, "轮回: 碎片/执念/轮回数/图鉴保留");
        CHECK(slimeRespawned, "轮回: 怪物重生");
    }

    printf("\n%s: %d 处失败\n", g_fail ? "自检未通过" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}

// ---------------- 主菜单 ----------------
static void showTitle() {
    term::clear();
    term::cursor(0);
    term::print(term::bold() + term::fg(3) +
                "        ┌─────────────────────────────┐\n"
                "        │       《 轮 回 之 塔 》       │\n"
                "        └─────────────────────────────┘\n" + term::reset());
    term::print(term::dim() + "  你救不了她。但你可以试一千次。\n\n" + term::reset());
}

static void mainMenu(Game& game, const std::string& savePath) {
    game.savePath = savePath; // P 键随时存档 / 暂停菜单保存共用此路径
    while (true) {
        if (ui::active() && ui::quitRequested()) break;
        showTitle();
        term::print("  1. 新轮回（开始）\n");
        term::print("  2. 继续（读档）\n");
        term::print("  3. 怪物图鉴\n");
        term::print("  4. 关于\n");
        term::print("  5. 退出\n\n");
        term::print(term::dim() + "  选择: " + term::reset());
        int k = term::readKey();
        if (k == '1') {
            game.newGame();
            if (game.scenes.count("intro")) game.runScene("intro");
            play(&game);
        } else if (k == '2') {
            int slot = game.saveSlotSelector(false);
            if (slot >= 0) {
                if (game.loadGame(slot)) {
                    play(&game);
                } else {
                    term::print(term::fg(1) + "\n  读取失败（存档  " + std::to_string(slot + 1) + " 无法载入）。\n" + term::reset());
                    term::anyKey();
                }
            } else {
                term::print(term::dim() + "\n  已取消读取。\n" + term::reset());
                term::anyKey();
            }
        } else if (k == '3') {
            game.showCodex(false);
        } else if (k == '4') {
            term::clear();
            term::cursor(0);
            term::print("《轮回之塔》 — C++ 魔塔 试玩版\n\n");
            term::print("把「死亡读档」本身写成剧情：\n");
            term::print("  · 死亡＝轮回，只有图鉴/探索/碎片/执念保留\n");
            term::print("  · 经典战斗公式 + 6 种怪物技能\n");
            term::print("  · 数据全外置（data/ 下的 JSON 与地图）\n");
            term::print("  · 50 层 / 四结局 / 轮回彩蛋：正式版见\n\n");
            term::anyKey();
        } else if (k == '5' || k == 0x1B) {
            term::print("\n  再见。塔会等你的。\n");
            break;
        }
    }
}

int main(int argc, char** argv) {
    term::init();
    std::string dataDir = findDataDir();
    std::string savePath = exeDir() + "\\save";   // 六槽存档前缀（save_0.dat .. save_5.dat）

    if (argc > 1 && std::string(argv[1]) == "--selftest")
        return runSelftest(dataDir);

    // 模式解析：默认图形窗口；--script 隐藏窗口跑 GUI；--console 回退终端
    bool wantGui = true;
    bool scriptMode = (argc > 2 && std::string(argv[1]) == "--script");
    if (argc > 1 && std::string(argv[1]) == "--console") wantGui = false;
    // ---- 槽位选择器渲染验证：--shot out.bmp slot（临时造 2 个存档槽渲染一帧） ----
    if (argc > 1 && std::string(argv[1]) == "--shot") {
        if (!ui::init(false)) return 1;
        term::g_gui = true;
        Game g;
        std::string e2;
        if (!g.init(dataDir, e2)) { printf("init fail: %s\n", e2.c_str()); return 1; }
        g.suppressScenes = true;
        g.newGame();
        if (argc > 3 && std::string(argv[3]) == "slot") {
            std::string old = g.savePath;
            g.savePath = exeDir();
            g.tryMove(3); g.tryMove(3);
            g.loopCount = 3; g.hero.fragments = 2; g.obsession = 9;
            if (!g.saveGame(0)) { printf("slot save0 FAILED\n"); return 3; }
            g.loopCount = 7; g.hero.fragments = 5;
            g.player = { 8, 1 };
            if (!g.saveGame(1)) { printf("slot save1 FAILED\n"); return 3; }
            g.savePath = old;
            Game::SaveMeta metas[6];
            for (int i = 0; i < 6; i++) g.readSaveMeta(i, metas[i]);
            g.drawSaveSlotFrame(true, 1, metas, "");
            ui::present();
            bool ok = ui::writeFrameBmp(argv[2]);
            for (int i = 0; i < 6; i++) remove((exeDir() + "_" + std::to_string(i) + ".dat").c_str());
            printf(ok ? "slot shot ok: %s\n" : "slot shot FAILED\n", argv[2]);
            ui::shutdown();
            return ok ? 0 : 2;
        }
        g.tryMove(3); g.tryMove(3); g.tryMove(3); g.tryMove(3); g.tryMove(3); // 走到黄钥匙旁
        g.render();
        ui::present();
        g.player = {8, 1};
        g.render();
        ui::present();
        bool ok = ui::writeFrameBmp(argv[2]);
        printf(ok ? "shot ok: %s\n" : "shot FAILED\n", argv[2]);
        ui::shutdown();
        return ok ? 0 : 2;
    }
    if (wantGui)
        if (ui::init(!scriptMode)) term::g_gui = true;

    Game game;
    std::string err;
    if (!game.init(dataDir, err)) {
        term::print("数据加载失败: " + err + "\n");
        term::print("请确认 data 目录与可执行文件在同一位置。\n");
        return 1;
    }
    // 脚本化冒烟：--script xx.txt（从主菜单开始按键）
    if (scriptMode) {
        loadScript(argv[2]);
        // 剧情已改为逐节点“按任意键继续”：脚本模式跳过剧情播放，
        // 避免每个节点吞掉一个注入键导致移动序列错位。
        game.suppressScenes = true;
        mainMenu(game, savePath);
        ui::shutdown();
        return 0;
    }
    mainMenu(game, savePath);
    ui::shutdown();
    return 0;
}