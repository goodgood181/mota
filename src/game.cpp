// game.cpp —— 游戏主逻辑实现
#include "game.h"
#include "battle.h"
#include "term.h"
#include "ui.h"
#include "sprites.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>
#include <set>

#ifdef _WIN32
#include <windows.h>
#endif

// ---------- 工具 ----------
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

// 按“显示宽度”（CJK=2 列）自动换行由 term::wrapText 提供（term.h）

// ---------- 初始化 ----------
bool Game::init(const std::string& dataDir, std::string& err) {
    if (!data.loadAll(dataDir, err)) return false;
    if (!loadEvents(dataDir + "\\events.json", scenes, err)) return false;
    // 楼层顺序
    jx::Json root;
    if (!jx::parseFile(dataDir + "\\floors.json", root, err)) return false;
    for (auto& f : root.arr) floorOrder.push_back(f.strVal("id"));
    if (floorOrder.empty()) { err = "没有楼层配置"; return false; }
    return true;
}

int Game::floorIndex() const {
    for (size_t i = 0; i < floorOrder.size(); i++)
        if (floorOrder[i] == cur) return (int)i;
    return 0;
}
int Game::floorNumber() const { return floorIndex() + 1; }
Floor& Game::floor() { return floors[cur]; }
const Floor& Game::floor() const { return floors.at(cur); }

// ---------- 新游戏 / 轮回 ----------
void Game::newGame() {
    loopCount = 0;
    obsession = 0;
    codex.clear();
    unlockedEndings.clear();
    hero = Hero{};
    hero.st = Stats{300, 20, 10, 0, 0};
    relicId.clear();
    flagExtraKey = false;
    resetForLoop();
}

// 当前遗物的效果键（relicId → items.json 的 relic_effect；无遗物返回空串）
std::string Game::relicEffect() const {
    if (relicId.empty()) return "";
    const ItemDef* it = data.item(relicId);
    return it ? it->relicEffect : "";
}

void Game::resetForLoop() {
    // 重建楼层（首次）或轮回重置
    for (auto& id : floorOrder) {
        auto it = floors.find(id);
        if (it == floors.end()) {
            std::string err;
            Floor f;
            if (!Floor::create(data.floors[id], data, f, err)) continue;
            floors[id] = std::move(f);
        } else {
            it->second.resetLoop();
        }
    }
    // 勇者重置（保留碎片/旗标/执念；经验与经验突破的永久成长跨轮回保留）
    int frags = hero.fragments;
    int expKeep = hero.st.exp;
    hero = Hero{};
    hero.fragments = frags;
    hero.st = Stats{300 + permHp, 20 + permAtk, 10 + permDef, 0, expKeep};
    if (expKeep > 0) addLog("轮回带走了记忆，却没带走肌肉的记忆：经验 +" + std::to_string(expKeep));
    if (permAtk > 0 || permDef > 0 || permHp > 0)
        addLog("经验突破保留：攻+" + std::to_string(permAtk) + " 防+" + std::to_string(permDef) +
               " 血+" + std::to_string(permHp));
    // 遗物初始效果（按 items.json 的 relic_effect 键分派）
    std::string eff = relicEffect();
    if (eff == "start_gold30") hero.st.gold += 150;
    if (eff == "start_hp100") hero.st.hp += 300;
    battlesThisLoop = 0;
    shopCount[0] = shopCount[1] = shopCount[2] = 0;
    extraKeyGranted.clear();
    log.clear();
    message.clear();
    markMode = false;
    // 回到 1F
    cur = floorOrder[0];
    player = floors[cur].start;
    facing = 3;
    floors[cur].markExplored(player.x, player.y);
    // 执念：每层开局多1把黄钥匙
    if (flagExtraKey && !extraKeyGranted.count(cur)) {
        hero.keys[KEY_YELLOW]++;
        extraKeyGranted.insert(cur);
        addLog("执念之钥生效：本层开局 +1 黄钥匙");
    }
}

// ---------- 上下文 ----------
CondContext Game::makeCtx() const {
    static std::map<std::string, int> kills; // 单线程游戏，允许静态复用
    kills.clear();
    for (auto& kv : codex)
        if (kv.second.kills > 0) kills[kv.first] = kv.second.kills;
    CondContext c;
    c.loop = loopCount;
    c.fragments = hero.fragments;
    c.obsession = obsession;
    c.gold = hero.st.gold;
    c.exp = hero.st.exp;
    c.atk = hero.st.atk;
    c.def = hero.st.def;
    c.floor = floorNumber();
    c.flags = &hero.flags;
    c.kills = &kills;
    return c;
}

// ---------- 日志 ----------
void Game::addLog(const std::string& s) {
    log.push_front(s);
    if ((int)log.size() > 5) log.pop_back();
}

// ---------- 主线指引 ----------
std::string Game::goalText() const {
    const Floor& fl = floor();
    std::string g = data.floors.count(cur) ? data.floors.at(cur).goal : "";
    if (g.empty()) g = "继续往塔顶前进";
    // 动态：本章 Boss 未死 → 提示强弱；已死 → 提示继续向上
    for (auto& kv : fl.monsters) {
        const MonsterDef* md = data.monster(kv.second.id);
        if (!md || !md->boss) continue;
        bool alive = kv.second.alive;
        if (alive) {
            double aMul = 1.0, dMul = 1.0;
            isAuraBoosted(kv.first, aMul, dMul);
            double atkMul = (loopCount >= 15) ? 1.1 : 1.0;
            bool heroFirst = (relicEffect() == "first_battle_firststrike" && battlesThisLoop == 0);
            BattleResult r = BattleSimulator::simulate(hero.st.hp, hero.st.atk, hero.st.def,
                                                       md->hp, md->atk, md->def, md->skills,
                                                       aMul, dMul, heroFirst, atkMul);
            if (!r.heroWins)
                g += "（以现在的力量必败——按 U 消耗经验突破攻防，或在商店购买强化）";
        } else {
            g = "本章之守已被击败。继续向上！";
        }
        break;
    }
    int idx = floorIndex();
    if (idx < 5) g = "【第 1 章】 " + g;
    else        g = "【第 2 章 · 镜之回廊】 " + g;
    return g;
}

// ---------- 移动与交互 ----------
void Game::handleKey(int key) {
    if (markMode) {
        // 标记暗墙模式
        switch (key) {
            case 0x11: markCursor.y = std::max(0, markCursor.y - 1); break;
            case 0x12: markCursor.y = std::min(floor().h - 1, markCursor.y + 1); break;
            case 0x13: markCursor.x = std::max(0, markCursor.x - 1); break;
            case 0x14: markCursor.x = std::min(floor().w - 1, markCursor.x + 1); break;
            case 0x0D: case ' ': case 'j': case 'J': {
                Floor& fl = floor();
                Point p = markCursor;
                if (fl.tile(p.x, p.y) == '#' && !fl.markedWalls.count(p) && obsession >= 5) {
                    obsession -= 5;
                    fl.markedWalls[p] = true;
                    addLog("你用 5 点执念，在灵魂中烙下一面暗墙的位置。");
                } else if (fl.tile(p.x, p.y) != '#') {
                    message = "只能标记普通墙(＃)";
                } else {
                    message = "该墙已被标记";
                }
                markMode = false;
                break;
            }
            case 0x1B: case 'q': case 'Q': markMode = false; break;
        }
        return;
    }

    switch (key) {
        case 0x11: case 'w': case 'W': tryMove(0); break;
        case 0x12: case 's': case 'S': tryMove(1); break;
        case 0x13: case 'a': case 'A': tryMove(2); break;
        case 0x14: case 'd': case 'D': tryMove(3); break;
        case 0x0D: case ' ': case 'k': case 'K': {
            // 攻击朝向怪物
            Point t = player + dirDelta(facing);
            Floor& fl = floor();
            auto mit = fl.monsters.find(t);
            if (mit != fl.monsters.end() && mit->second.alive) {
                battleAt(t, true);
            } else if (fl.tile(t.x, t.y) == 'n') {
                talkNpc();
            } else if (fl.tile(t.x, t.y) == 's') {
                openShop();
            } else if (fl.isHiddenWall(t.x, t.y)) {
                tryMove(facing);
            } else {
                message = "那里没有敌人";
            }
            break;
        }
        case 'h': case 'H': markWallMode(); break;
        case 'i': case 'I': showCodex(true); break;
        case 'e': case 'E': openShop(); break;   // 站在商店旁交互
        case 'u': case 'U': shopExpMenu(); break; // 成长之殿：经验突破（随时可点）
        case 'p': case 'P': {                      // 随时存档（六格选槽）
            int slot = saveSlotSelector(true);
            if (slot >= 0) {
                if (saveGame(slot)) {
                    addLog("已保存到 存档 " + std::to_string(slot + 1) + "。");
                    message = "已保存（存档 " + std::to_string(slot + 1) + "）";
                } else {
                    message = "存档失败";
                }
            }
            break;
        }
        case 0x1B: pauseMenu(); break;
        default: break;
    }
}

void Game::tryMove(int dir) {
    facing = dir;
    Point t = player + dirDelta(dir);
    Floor& fl = floor();
    if (!fl.inBounds(t.x, t.y)) return;

    // 隐藏墙：撞开
    if (fl.isHiddenWall(t.x, t.y)) {
        fl.grid[t.y][t.x] = '.';
        player = t;
        fl.markExplored(t.x, t.y);
        addLog("你撞开了一面暗墙！后面是……");
        checkStep(t);
        return;
    }
    // 怪物
    auto mit = fl.monsters.find(t);
    if (mit != fl.monsters.end() && mit->second.alive) {
        const MonsterDef* md = data.monster(mit->second.id);
        if (md && loopCount >= 10 && md->spare) {
            // 让路
            for (int d = 0; d < 4; d++) {
                Point np = t + dirDelta(d);
                if (fl.inBounds(np.x, np.y) && !fl.solid(np.x, np.y) &&
                    fl.monsters.find(np) == fl.monsters.end() && !(np == player)) {
                    fl.monsters[np] = fl.monsters[t];
                    fl.monsters.erase(t);
                    if (!md->soulLine.empty()) addLog(md->name + "：" + md->soulLine);
                    addLog(md->name + "退开一步，让出道路。（轮回十次后的眼神）");
                    player = t;
                    fl.markExplored(t.x, t.y);
                    checkStep(t);
                    return;
                }
            }
        }
        // 普通战斗（预览 + 确认）
        if (battleAt(t, true)) {
            player = t;
            fl.markExplored(t.x, t.y);
            checkStep(t);
        }
        return;
    }
    // 门
    if (fl.isDoor(t.x, t.y)) {
        int k = fl.doorColor(t.x, t.y);
        if (hero.keys[k] > 0) {
            hero.keys[k]--;
            fl.grid[t.y][t.x] = '=';
            player = t;
            fl.markExplored(t.x, t.y);
            addLog(std::string("使用") + keyName(k) + "打开了门。");
            checkStep(t);
        } else {
            message = "缺少" + std::string(keyName(k));
            addLog("门锁着。需要" + std::string(keyName(k)) + "。");
        }
        return;
    }
    // 商店/NPC：交互后可穿过（经典魔塔行为）
    if (fl.tile(t.x, t.y) == 's') openShop();
    if (fl.tile(t.x, t.y) == 'n') talkNpc();
    // 普通墙
    if (fl.solid(t.x, t.y)) { message = "墙"; return; }
    // 可走
    player = t;
    fl.markExplored(t.x, t.y);
    // 道具
    auto iit = fl.items.find(t);
    if (iit != fl.items.end() && !iit->second.taken) pickupItem(t);
    checkStep(t);
}

// 落脚后的事件（触发点/楼梯）
void Game::checkStep(const Point& t) {
    Floor& fl = floor();
    char c = fl.tile(t.x, t.y);
    if (c == 'T') {
        triggerStep();
        return;
    }
    if (c == '>' ) { changeFloor(true); return; }
    if (c == '<') { changeFloor(false); return; }
}

// ---------- 拾取 ----------
void Game::pickupItem(const Point& p) {
    Floor& fl = floor();
    ItemInst& ii = fl.items[p];
    if (ii.taken) return;
    ii.taken = true;
    fl.grid[p.y][p.x] = '.';
    const ItemDef* d = data.item(ii.id);
    if (!d) { addLog("未知道具: " + ii.id); return; }
    if (d->type == "hp") {
        hero.st.hp += d->value;
        addLog("获得" + d->name + "，生命 +" + std::to_string(d->value));
    } else if (d->type == "key") {
        int kc = d->value;
        if (kc < 0 || kc > 2) kc = 0;
        hero.keys[kc]++;
        addLog("获得【" + std::string(keyName(kc)) + "】");
    } else if (d->type == "atk") {
        hero.st.atk += d->value;
        addLog("获得" + d->name + "，攻击 +" + std::to_string(d->value));
    } else if (d->type == "def") {
        hero.st.def += d->value;
        addLog("获得" + d->name + "，防御 +" + std::to_string(d->value));
    } else if (d->type == "gold") {
        hero.st.gold += d->value;
        addLog("获得" + d->name + "，金币 +" + std::to_string(d->value));
    } else if (d->type == "fragment") {
        hero.fragments++;
        std::string txt = !d->pickupText.empty() ? d->pickupText : "获得记忆碎片。";
        addLog("获得【记忆碎片】(" + std::to_string(hero.fragments) + "/7)");
        message = txt;
    } else if (d->type == "relic") {
        relicId = d->id;
        addLog("获得遗物【" + d->name + "】");
        // 遗物进场效果
        if (d->relicEffect == "start_hp100") hero.st.hp += 100;
        if (d->relicEffect == "start_gold30") hero.st.gold += 30;
    } else {
        addLog("获得" + d->name);
    }
    if (!d->pickupText.empty() && d->type != "fragment") message = d->pickupText;
}

// ---------- 换层 ----------
void Game::changeFloor(bool up) {
    int idx = floorIndex();
    int next = up ? idx + 1 : idx - 1;
    if (next < 0 || next >= (int)floorOrder.size()) {
        if (up) checkEndings(true);
        else message = "没有更低的楼层了";
        return;
    }
    cur = floorOrder[next];
    Floor& fl = floors[cur];
    // 进入点：上楼→目标层的下楼口；下楼→目标层的上楼口
    Point enter = up ? fl.downStair : fl.upStair;
    if (enter.x < 0) enter = {1, 1};
    player = enter;
    facing = 3;
    fl.markExplored(player.x, player.y);
    addLog("来到 " + std::to_string(next + 1) + "F " + data.floors[cur].name);
    // 执念：每层开局多1把黄钥匙
    if (flagExtraKey && !extraKeyGranted.count(cur)) {
        hero.keys[KEY_YELLOW]++;
        extraKeyGranted.insert(cur);
        addLog("执念之钥生效：本层开局 +1 黄钥匙");
    }
    // 楼层进入事件
    auto sit = scenes.find("enter_" + cur);
    if (sit != scenes.end() && scenePasses(sit->second, makeCtx()) && !hero.hasFlag("seen_enter_" + cur)) {
        runScene("enter_" + cur);
    }
}

// ---------- 战斗 ----------
bool Game::isAuraBoosted(const Point& pos, double& atkMul, double& defMul) const {
    const Floor& fl = floor();
    bool any = false;
    for (int d = 0; d < 4; d++) {
        Point np = pos + dirDelta(d);
        auto it = fl.monsters.find(np);
        if (it != fl.monsters.end() && it->second.alive) {
            const MonsterDef* md = data.monster(it->second.id);
            if (md && BattleSimulator::hasSkill(md->skills, SKILL_AURA)) {
                atkMul *= 1.3;
                defMul *= 1.3;
                any = true;
            }
        }
    }
    return any;
}

bool Game::battleAt(const Point& pos, bool confirm) {
    Floor& fl = floor();
    auto mit = fl.monsters.find(pos);
    if (mit == fl.monsters.end() || !mit->second.alive) return false;
    const MonsterDef* md = data.monster(mit->second.id);
    if (!md) return false;

    codex[md->id].seen = true;

    double aMul = 1.0, dMul = 1.0;
    bool auraAny = isAuraBoosted(pos, aMul, dMul);
    double atkMul = (loopCount >= 15) ? 1.1 : 1.0;  // 塔的不耐烦
    bool heroFirst = (relicEffect() == "first_battle_firststrike" && battlesThisLoop == 0);

    BattleResult r = BattleSimulator::simulate(
        hero.st.hp, hero.st.atk, hero.st.def,
        md->hp, md->atk, md->def, md->skills,
        aMul, dMul, heroFirst, atkMul);

    // 预览确认（脚本模式由脚本按键确认；注意：自检路径须避开活怪，勿依赖此处自动跳过）
    if (confirm) {
        // 每轮循环头重画（present 会清空缓冲；无效键如方向键后必须重画，
        // 否则下一次 readKey 上屏最后一帧就是空白）
        auto drawPreview = [&]() {
            term::clear();
            term::cursor(0);
            term::print(term::bold() + term::fg(4) + "———— 战斗预览 ————" + term::reset() + "\n\n");
            std::string skillsCn;
            for (auto& s : md->skills) {
                Skill sk = BattleSimulator::parse(s);
                const char* cn = BattleSimulator::cnName(sk);
                if (*cn) { if (!skillsCn.empty()) skillsCn += " "; skillsCn += cn; }
            }
            term::print(term::fg(1) + md->name + term::reset() +
                        (md->boss ? term::bold() + std::string("[Boss]") + term::reset() : "") + "\n");
            term::print("生命 " + std::to_string(md->hp) + "  攻击 " + std::to_string((int)(md->atk * (loopCount >= 15 ? 1.1 : 1.0))) +
                        "  防御 " + std::to_string(md->def) + "\n");
            if (auraAny) term::print(term::fg(3) + "（领域怪在旁，攻防+30%）" + term::reset() + "\n");
            if (!skillsCn.empty())
                term::print(term::fg(5) + "技能: " + skillsCn + term::reset() + "\n\n");
            if (r.heroWins) {
                term::print("你的攻击 " + std::to_string(r.heroDmgPerRound) + "/回合，击杀约 " + std::to_string(r.rounds) + " 回合\n");
                term::print("预计损血 " + term::fg(2) + std::to_string(r.heroLossHp) + term::reset() + "（当前生命 " + std::to_string(hero.st.hp) + "）\n");
            } else {
                term::print(term::bold() + term::fg(1) + "你会死在这里。损血 " + std::to_string(r.heroLossHp) +
                            " 远超当前生命 " + std::to_string(hero.st.hp) + term::reset() + "\n");
            }
            term::print(term::dim() + "\n获胜获得: 金币 " + std::to_string(md->gold * (loopCount >= 15 ? 2 : 1)) +
                        "  经验 " + std::to_string(md->exp) + term::reset() + "\n\n");
            term::print(term::dim() + "[Enter] 进攻    [Esc] 撤退" + term::reset());
        };
        for (;;) {
            drawPreview();
            int k = term::readKey();
            if (k == 0x1B) return false;
            if (k == 0x0D || k == ' ' || k == 'k' || k == 'K') break;
        }
    }

    if (!r.heroWins) {
        die(md->name);
        return false;
    }
    applyBattleWin(md->id, pos, BattleOutcome{true, r.heroLossHp, r.rounds, r.curseAtkLoss, heroFirst});
    return true;
}

void Game::applyBattleWin(const std::string& mid, const Point& pos, const BattleOutcome& o) {
    Floor& fl = floor();
    const MonsterDef* md = data.monster(mid);
    hero.st.hp -= o.lossHp;
    int gold = md->gold;
    if (loopCount >= 15) gold *= 2;
    if (relicEffect() == "double_gold") gold *= 2;
    hero.st.gold += gold;
    hero.st.exp += md->exp;
    battlesThisLoop++;

    if (o.curseAtk > 0) {
        hero.st.atk -= o.curseAtk;
        if (hero.st.atk < 1) hero.st.atk = 1;
        addLog("诅咒发作！攻击永久 -" + std::to_string(o.curseAtk));
    }
    auto& ce = codex[mid];
    ce.kills++;
    if (ce.minHpLoss < 0 || o.lossHp < ce.minHpLoss) ce.minHpLoss = o.lossHp;

    fl.monsters[pos].alive = false;
    // 分裂
    if (BattleSimulator::hasSkill(md->skills, SKILL_SPLIT) && !md->splitInto.empty()) {
        std::vector<Point> spawns;
        for (int d = 0; d < 4 && (int)spawns.size() < 2; d++) {
            Point np = pos + dirDelta(d);
            if (fl.inBounds(np.x, np.y) && !fl.solid(np.x, np.y) &&
                fl.monsters.find(np) == fl.monsters.end() && !(np == player)) {
                spawns.push_back(np);
            }
        }
        for (auto& sp : spawns) {
            fl.monsters[sp] = MonsterInst{md->splitInto, true, '!'};
        }
        if (!spawns.empty())
            addLog(md->name + "裂开了！分裂出 " + std::to_string(spawns.size()) + " 只小的！");
    }
    if (o.lossHp > 0)
        addLog("击败" + md->name + "，损血 " + std::to_string(o.lossHp) +
               "，获得 金币" + std::to_string(gold) + " 经验" + std::to_string(md->exp));
    else
        addLog("无伤击败" + md->name + "！获得 金币" + std::to_string(gold) + " 经验" + std::to_string(md->exp));
    if (loopCount >= 10 && !md->soulLine.empty()) {
        // 灵魂台词
        message = md->name + "：" + md->soulLine;
    }
}

// ---------- 对话 ----------
void Game::runScene(const std::string& id) {
    if (suppressScenes) { hero.setFlag("seen_" + id); return; }
    auto it = scenes.find(id);
    if (it == scenes.end()) { addLog("事件缺失: " + id); return; }
    DialogScene& sc = it->second;
    CondContext ctx = makeCtx();

    std::vector<DialogNode*> active;
    for (auto& n : sc.nodes) {
        if (n.when.empty() || evalCondInner(n.when, ctx)) active.push_back(&n);
    }
    if (active.empty()) { addLog("（此处只有沉默…）"); return; }

    if (sc.cinema) {
        // 剧情一律“按任意键继续”（不再自动播放，避免文本过快看不清）
        term::clear();
        term::cursor(0);
        term::print(term::dim() + "　　　……忽然，世界安静下来。\n" + term::reset());
        term::anyKey(term::dim() + "（按任意键继续……）" + term::reset());
        for (auto* n : active) {
            term::clear();
            std::string txt = fillPlaceholders(n->text, ctx);
            term::print(term::dim() + "……\n\n" + term::reset());
            if (!n->who.empty())
                term::print(term::bold() + term::fg(6) + n->who + term::reset() + "\n\n");
            term::print(txt + "\n\n");
            if (!n->set_flag.empty()) hero.setFlag(n->set_flag);
            term::anyKey(term::dim() + "（按任意键继续……）" + term::reset());
        }
        term::anyKey(term::dim() + "（黑暗中，你仍在呼吸……）" + term::reset());
        return;
    }

    // 普通对话流
    size_t i = 0;
    std::set<std::string> visited;
    while (i < active.size()) {
        DialogNode* n = active[i];
        if (visited.count(n->id)) break;
        visited.insert(n->id);
        std::string txt = fillPlaceholders(n->text, ctx);
        term::clear();
        term::cursor(0);
        if (!n->who.empty())
            term::print(term::bold() + term::fg(6) + n->who + term::reset() + term::dim() + "：" + term::reset() + "\n\n");
        term::print(txt + "\n\n");

        int next = (int)i + 1;
        bool endScene = false;

        if (!n->choices.empty()) {
            std::vector<const DialogChoice*> avail;
            for (auto& ch : n->choices)
                if (ch.require.empty() || evalCondInner(ch.require, ctx)) avail.push_back(&ch);
            if (avail.empty()) endScene = true;
            while (!endScene) {
                for (size_t k = 0; k < avail.size(); k++) {
                    const DialogChoice* ch = avail[k];
                    std::string mark = "· ";
                    term::print(term::fg(6) + mark + term::reset() + std::to_string(k + 1) + ". " + ch->text + "\n");
                }
                term::print(term::dim() + "\n选择 (1-" + std::to_string(avail.size()) + "): " + term::reset());
                int key = term::readKey();
                if (key >= '1' && key <= '9') {
                    int idx = key - '1';
                    if (idx < (int)avail.size()) {
                        const DialogChoice* ch = avail[idx];
                        if (!ch->set_flag.empty()) hero.setFlag(ch->set_flag);
                        if (!ch->goto_n.empty() && ch->goto_n != "end") {
                            bool found = false;
                            for (size_t j = 0; j < active.size(); j++)
                                if (active[j]->id == ch->goto_n) { next = (int)j; found = true; break; }
                            if (!found) endScene = true;
                        } else endScene = true;
                        if (endScene && !ch->goto_n.empty() && ch->goto_n == "end") endScene = true;
                        next = endScene ? (int)active.size() : next;
                        break;
                    }
                }
            }
        } else {
            if (!n->set_flag.empty()) hero.setFlag(n->set_flag);
            if (!n->goto_n.empty() && n->goto_n != "end") {
                bool found = false;
                for (size_t j = 0; j < active.size(); j++)
                    if (active[j]->id == n->goto_n) { next = (int)j; found = true; break; }
                if (!found) endScene = true;
            }
            if (endScene) next = (int)active.size();
        }
        i = (size_t)next;
        if (i < active.size()) {
            term::anyKey();
            term::clear();
        }
    }
    if (sc.once) hero.setFlag("seen_" + sc.id);
}

void Game::talkNpc() {
    Floor& fl = floor();
    Point np = player + dirDelta(facing);
    if (fl.tile(np.x, np.y) == 'n') { talkNpcAt(np); return; }
    // 也检查四周（防止朝向偏差）
    for (int d = 0; d < 4; d++) {
        Point np2 = player + dirDelta(d);
        if (fl.tile(np2.x, np2.y) == 'n') { talkNpcAt(np2); return; }
    }
    message = "这里没有人";
}
void Game::talkNpcAt(const Point& np) {
    Floor& fl = floor();
    std::string ev = fl.npcEvent;
    if (ev.empty()) { message = "他似乎无话可说。"; return; }
    // 冒号分隔的候补事件列表：取第一个满足触发条件的
    std::vector<std::string> ids;
    {
        size_t s = 0, pos;
        while ((pos = ev.find(';', s)) != std::string::npos) {
            ids.push_back(ev.substr(s, pos - s));
            s = pos + 1;
        }
        ids.push_back(ev.substr(s));
    }
    for (auto& id : ids) {
        auto it = scenes.find(id);
        if (it == scenes.end()) continue;
        if (scenePasses(it->second, makeCtx())) {
            runScene(id);
            return;
        }
    }
    message = "他似乎对你不感兴趣。";
}

// ---------- 触发点 ----------
void Game::triggerStep() {
    Floor& fl = floor();
    if (fl.triggerEvent.empty()) return;
    if (hero.hasFlag("seen_" + fl.triggerEvent)) return;
    auto it = scenes.find(fl.triggerEvent);
    if (it != scenes.end() && scenePasses(it->second, makeCtx())) {
        runScene(fl.triggerEvent);
    }
    // 触发点一次性：玩家踩过的这一格消失（其余保留）
    if (fl.inBounds(player.x, player.y) && fl.grid[player.y][player.x] == 'T')
        fl.grid[player.y][player.x] = '.';
}

// ---------- 死亡与轮回 ----------
void Game::die(const std::string& reason) {
    addLog("你倒下了——被 " + reason + " 杀死。");
    term::clear();
    term::cursor(0);
    term::print(term::fg(1) + "你倒下了……" + term::reset() + "\n");
    term::sleepMs(300);
    loopCount++;
    obsession += 10;
    if (scenes.count("death_narration")) {
        runScene("death_narration");
    } else {
        term::clear();
        term::cursor(0);
        term::print("黑暗。然后，光。\n\n你站在塔门前，村庄完好无损。\n脑中的声音温柔依旧：\n\n  \"去吧。她在顶层等你。\"\n");
        term::anyKey();
    }
    deathScreen();
}

void Game::deathScreen() {
    while (true) {
        term::clear();
        term::cursor(0);
        term::print(term::bold() + term::fg(4) + "———— 轮回结算 ————" + term::reset() + "\n\n");
        term::print("第 " + std::to_string(loopCount) + " 次轮回即将开始\n");
        term::print("你保留了：怪物图鉴 / 已探索地图 / 记忆碎片(" + std::to_string(hero.fragments) + "/7) / 执念\n\n");
        term::print(term::fg(3) + "执念：" + std::to_string(obsession) + term::reset() + "\n");
        term::print("（死亡本身也是信息：5点可标记暗墙、10点每层+1黄钥匙，20点从第零层带走遗物）\n\n");
        term::print(" 1. 继续轮回（回到 1F）\n");
        if (loopCount >= 10 && obsession >= 20)
            term::print(term::fg(6) + " 2. 从第零层带走一件遗物（-20 执念）" + term::reset() + "\n");
        else if (loopCount < 10)
            term::print(term::dim() + " 2. 从第零层带走遗物（轮回 ≥10 解锁）" + term::reset() + "\n");
        else
            term::print(term::dim() + " 2. 从第零层带走遗物（执念不足 20）" + term::reset() + "\n");
        if (!flagExtraKey && obsession >= 10)
            term::print(term::fg(6) + " 3. 执念强化：每层开局 +1 黄钥匙（-10 执念）" + term::reset() + "\n");
        else
            term::print(term::dim() + " 3. 执念强化：每层开局 +1 黄钥匙（-10 执念" +
                        (flagExtraKey ? "，已强化" : "，执念不足") + "）" + term::reset() + "\n");
        term::print(" 4. 查看怪物图鉴\n");
        term::print(term::dim() + "\n选择: " + term::reset());
        int k = term::readKey();
        if (k == '1') break;
        if (k == '2' && loopCount >= 10 && obsession >= 20) {
            if (chooseRelic()) break;   // 选完遗物直接开启新轮回（不再停留结算界面）
            continue;                    // 放弃则留在结算界面
        }
        if (k == '3' && !flagExtraKey && obsession >= 10) {
            obsession -= 10;
            flagExtraKey = true;
            term::clear();
            term::cursor(0);
            term::print(term::fg(6) + "塔的规则在你的记忆中松动了一角。\n此后每个轮回，每层开局你都会多带 1 把黄钥匙。\n\n" + term::reset());
            term::anyKey();
            continue;
        }
        if (k == '4') { showCodex(true); continue; }
    }
    resetForLoop();
}

bool Game::chooseRelic() {
    std::vector<const ItemDef*> relics;
    for (auto& kv : data.items)
        if (kv.second.type == "relic") relics.push_back(&kv.second);
    if (relics.empty()) { message = "没有可选的遗物"; return false; }
    // 循环头重画（present 清空缓冲，无效键后重画避免空白帧）
    for (;;) {
        term::clear();
        term::cursor(0);
        term::print(term::bold() + term::fg(4) + "———— 第零层 · 历代勇者的墓地 ————" + term::reset() + "\n\n");
        term::print("你只允许带走一件。其余，留给下一个你。\n\n");
        for (size_t i = 0; i < relics.size(); i++) {
            const ItemDef* r = relics[i];
            term::print(" " + std::to_string(i + 1) + ". " + term::fg(3) + r->name + term::reset() + " — " + r->desc + "\n");
        }
        term::print(term::dim() + "\n选择 (1-" + std::to_string(relics.size()) + ")，Esc 放弃: " + term::reset());
        int k = term::readKey();
        if (k == 0x1B) return false;
        if (k >= '1' && k <= '9') {
            int idx = k - '1';
            if (idx < (int)relics.size()) {
                obsession -= 20;
                relicId = relics[idx]->id;
                term::clear();
                term::cursor(0);
                term::print(term::fg(3) + "你拾起了【" + relics[idx]->name + "】。\n\n" + term::reset());
                term::anyKey();
                return true;   // 已选定 → deathScreen 直接开启新轮回
            }
        }
    }
}

// ---------- 标签墙标记 ----------
void Game::markWallMode() {
    if (obsession < 5) { message = "执念不足 5，无法标记暗墙"; return; }
    markMode = true;
    markCursor = player;
    addLog("进入标记模式：移动光标到墙(＃)上，回车标记(-5执念)，Esc取消");
}

// ---------- 商店 ----------
int Game::shopPrice(int kind) const {
    if (kind == SHOP_GOLD_ATK || kind == SHOP_GOLD_DEF) return 25 * (shopCount[kind] + 1);
    return 10 * (shopCount[kind] + 1); // 血
}

void Game::openShop() {
    term::clear();
    term::cursor(0);
    term::print(term::bold() + term::fg(3) + "———— 商人 ————" + term::reset() + "\n\n");
    term::print("永远是同一张脸。他每次见到你，都像第一次。\n\n");
    while (true) {
        term::clear();
        term::cursor(0);
        term::print(term::bold() + term::fg(3) + "———— 商人 ————" + term::reset() + "\n\n");
        term::print("金币 " + std::to_string(hero.st.gold) + "　经验 " + std::to_string(hero.st.exp) + "\n\n");
        term::print(" 1. 力量 +4 攻　" + term::fg(2) + std::to_string(shopPrice(SHOP_GOLD_ATK)) + "金" + term::reset() + "\n");
        term::print(" 2. 护甲 +4 防　" + term::fg(2) + std::to_string(shopPrice(SHOP_GOLD_DEF)) + "金" + term::reset() + "\n");
        term::print(" 3. 大血瓶 +200血　" + term::fg(2) + std::to_string(shopPrice(SHOP_GOLD_HP)) + "金" + term::reset() + "\n");
        term::print(" 4. 经验突破 100经验 → 攻击/防御/生命 三选一\n");
        term::print(" 5. 离开\n");
        term::print(term::dim() + "\n选择: " + term::reset());
        int k = term::readKey();
        if (k == '1') shopBuy(SHOP_GOLD_ATK);
        else if (k == '2') shopBuy(SHOP_GOLD_DEF);
        else if (k == '3') shopBuy(SHOP_GOLD_HP);
        else if (k == '4') shopExpMenu();
        else if (k == '5' || k == 0x1B) break;
    }
}

void Game::shopBuy(int kind) {
    int price = shopPrice(kind);
    if (hero.st.gold < price) { message = "金币不足"; return; }
    hero.st.gold -= price;
    shopCount[kind]++;
    if (kind == SHOP_GOLD_ATK) { hero.st.atk += 4; addLog("商人让你更锋利了：攻击 +4"); }
    if (kind == SHOP_GOLD_DEF) { hero.st.def += 4; addLog("商人给你加了甲：防御 +4"); }
    if (kind == SHOP_GOLD_HP)  { hero.st.hp += 200; addLog("你喝下了大血瓶：生命 +200"); }
}

void Game::shopExpMenu() {
    if (hero.st.exp < 100) { message = "经验不足 100"; return; }
    // 循环头重画（present 清空缓冲，无效键后重画避免空白帧）
    for (;;) {
        term::clear();
        term::cursor(0);
        term::print(term::bold() + term::fg(5) + "经验突破" + term::reset() + "　消耗 100 经验（永久成长，轮回保留）：\n\n");
        term::print(" 1. 攻击 +25\n 2. 防御 +25\n 3. 生命 +1500\n\n" + term::dim() + "选择 (1-3), Esc 返回: " + term::reset());
        int k = term::readKey();
        int mode = -1;
        if (k == '1') mode = 0;
        else if (k == '2') mode = 1;
        else if (k == '3') mode = 2;
        else if (k == 0x1B) return;
        if (mode >= 0) {
            hero.st.exp -= 100;
            expShopFlags++;
            if (mode == 0) { hero.st.atk += 25; permAtk += 25; addLog("经验突破：攻击 +25（永久）"); }
            if (mode == 1) { hero.st.def += 25; permDef += 25; addLog("经验突破：防御 +25（永久）"); }
            if (mode == 2) { hero.st.hp += 1500; permHp += 1500; addLog("经验突破：生命 +1500（永久）"); }
            term::anyKey();
            return;
        }
    }
}

// ---------- 结局 ----------
void Game::checkEndings(bool atTop) {
    CondContext ctx = makeCtx();
    std::string chosen;
    for (auto& e : data.endings) {
        bool ok = true;
        for (auto& c : e.conds)
            if (!evalCondInner(c, ctx)) { ok = false; break; }
        if (ok) { chosen = e.id; break; }
    }
    if (chosen.empty()) {
        // 按当前最高楼层决定章节完结过场
        if (floorNumber() >= 20 && floorOrder.size() >= 20) runScene("chapter3_end");
        else runScene("chapter2_end");
        return;
    }
    unlockedEndings.push_back(chosen);
    runScene("ending_" + chosen);
    // 终幕：回到标题
    term::anyKey();
    running = false;
}

// ---------- 暂停菜单 ----------
void Game::pauseMenu() {
    // 循环头重画：present 清空缓冲，无效键（方向键/WASD）后必须重画，避免空白帧
    for (;;) {
        term::clear();
        term::cursor(0);
        term::print(term::bold() + "—— 暂停 ——" + term::reset() + "\n\n");
        term::print(" 1. 继续\n 2. 保存并退出到标题\n 3. 查看图鉴\n");
        term::print(term::dim() + "\n选择: " + term::reset());
        int k = term::readKey();
        if (k == '1' || k == 0x1B) return;
        if (k == '2') { int slot = saveSlotSelector(true); if (slot >= 0 && saveGame(slot)) { running = false; return; } }
        if (k == '3') { showCodex(true); return; }
    }
}

// ---------- 图鉴 ----------
void Game::showCodex(bool inGame) {
    term::clear();
    term::cursor(0);
    term::print(term::bold() + term::fg(4) + "———— 怪物图鉴 ————" + term::reset() + "\n\n");
    term::print("每一只怪物，都是上一次轮回中死去的你。\n\n");
    int n = 0;
    for (auto& kv : data.monsters) {
        const MonsterDef& md = kv.second;
        auto it = codex.find(md.id);
        bool seen = (it != codex.end());
        n++;
        if (!seen) { term::print(term::dim() + "???" + term::reset() + "\n"); continue; }
        std::string skillsCn;
        for (auto& s : md.skills) {
            const char* cn = BattleSimulator::cnName(BattleSimulator::parse(s));
            if (*cn) { if (!skillsCn.empty()) skillsCn += " "; skillsCn += cn; }
        }
        std::string line = md.name + "  HP" + std::to_string(md.hp) + " 攻" + std::to_string(md.atk) +
                           " 防" + std::to_string(md.def) + " 金" + std::to_string(md.gold) +
                           " 经" + std::to_string(md.exp) + (skillsCn.empty() ? "" : " [" + skillsCn + "]");
        if (it->second.kills > 0)
            term::print(line + term::fg(2) + "　击杀 " + std::to_string(it->second.kills) +
                        (it->second.minHpLoss >= 0 ? "（最佳损血" + std::to_string(it->second.minHpLoss) + "）" : "") + term::reset() + "\n");
        else
            term::print(line + "　(遭遇未战)\n");
        if (loopCount >= 10 && !md.soulLine.empty())
            term::print(term::fg(5) + "　  " + md.soulLine + term::reset() + "\n");
    }
    term::print(term::dim() + "\n共 " + std::to_string(n) + " 种。" + term::reset());
    term::anyKey();
    if (!inGame) {}
}

// ---------- 渲染 ----------
void Game::render() {
    term::clear();
    if (term::g_gui) { renderGui(); return; }
    term::cursor(0);
    const Floor& fl = floor();
    // 标题栏
    std::string title = term::bold() + "《轮回之塔》 " + term::reset() +
                        term::fg(3) + std::to_string(floorNumber()) + "F " + data.floors[cur].name +
                        term::reset() + term::dim() + "　轮回 " + std::to_string(loopCount) + " 次" + term::reset();
    term::print(title + "\n");

    // 地图 + 面板
    int panelW = 34;
    std::vector<std::string> panelLines;
    {
        std::stringstream ss(drawPanelText(panelW));
        std::string line;
        while (std::getline(ss, line)) panelLines.push_back(line);
    }
    for (int y = 0; y < fl.h; y++) {
        std::string row;
        for (int x = 0; x < fl.w; x++) drawTile(x, y, row);
        std::string ptext = (y < (int)panelLines.size()) ? panelLines[y] : "";
        term::print(row + ptext + "\n");
    }

    int logY = fl.h + 2;
    // 主线目标（terminal 版）：自动换行完整显示（不放一行，避免被终端右缘截断）
    {
        std::vector<std::string> lines = term::wrapText("▶ 目标：" + goalText(), std::max(20, term::g_cols - 8));
        int goLines = std::min((int)lines.size(), 3);
        for (int k = 0; k < goLines; k++) {
            term::setpos(0, logY - 1 + k);
            term::print(term::fg(3) + lines[k] + term::reset() + std::string(term::g_cols, ' '));
        }
    }
    // 日志：只显示最近 6 条，起点让出目标区（最多 3 行）
    {
        int start = (int)log.size() > 6 ? (int)log.size() - 6 : 0;
        for (size_t k = (size_t)start; k < log.size(); k++) {
            term::setpos(0, logY + 2 + (int)(k - (size_t)start));
            term::print(term::dim() + "· " + term::reset() + log[k] + "            ");
        }
    }
    // 朝向怪物预览
    Point t = player + dirDelta(facing);
    auto mit = fl.monsters.find(t);
    if (mit != fl.monsters.end() && mit->second.alive) {
        const MonsterDef* md = data.monster(mit->second.id);
        if (md) {
            double aMul = 1.0, dMul = 1.0;
            isAuraBoosted(t, aMul, dMul);
            double atkMul = (loopCount >= 15) ? 1.1 : 1.0;
            bool heroFirst = (relicEffect() == "first_battle_firststrike" && battlesThisLoop == 0);
            BattleResult r = BattleSimulator::simulate(hero.st.hp, hero.st.atk, hero.st.def,
                                                       md->hp, md->atk, md->def, md->skills,
                                                       aMul, dMul, heroFirst, atkMul);
            std::string line = term::fg(1) + md->name + term::reset() + " 损血" + std::to_string(r.heroLossHp) +
                               " 约" + std::to_string(r.rounds) + "回合 ";
            line += r.heroWins ? term::fg(2) + "可击败" + term::reset() : term::fg(1) + "会死" + term::reset();
            term::setpos(0, logY + 8);
            term::print(line);
        }
    }
    // 消息
    if (!message.empty()) {
        term::setpos(0, logY + 9);
        term::print(term::fg(5) + message + term::reset() + "            ");
        message.clear();
    }
    // 操作提示
    term::setpos(0, logY + 11);
    term::print(term::dim() + "WASD移动 · Enter攻击 · P存档 · U突破 · H标记 · I图鉴 · Esc菜单" + term::reset() + "     ");
}

// ---------- 图形模式整帧 ----------
void Game::renderGui() {
    term::clear();
    const Floor& fl = floor();
    ui::bg(0x1B1D23);

    // 标题
    std::string title = term::bold() + "《轮回之塔》 " + term::reset() +
                        term::fg(3) + std::to_string(floorNumber()) + "F " + data.floors[cur].name +
                        term::reset() + term::dim() + "　轮回 " + std::to_string(loopCount) + " 次" + term::reset();
    term::print(title + "\n");

    // 每格精灵选择
    auto tileSprite = [&](int x, int y) -> const spr::Sprite* {
        const char c = fl.tile(x, y);
        auto mit = fl.monsters.find({x, y});
        if (mit != fl.monsters.end() && mit->second.alive) {
            auto sm = spr::monsterSprite().find(mit->second.id);
            return sm != spr::monsterSprite().end() ? spr::byName(sm->second) : nullptr;
        }
        auto iit = fl.items.find({x, y});
        if (iit != fl.items.end() && !iit->second.taken) {
            auto si = spr::itemSprite().find(iit->second.id);
            return si != spr::itemSprite().end() ? spr::byName(si->second) : nullptr;
        }
        switch (c) {
            case '#': return &spr::spr_wall;
            case 'H': return &spr::spr_wall_h;
            case '1': return &spr::spr_door1;
            case '2': return &spr::spr_door2;
            case '3': return &spr::spr_door3;
            case '>': return &spr::spr_stair_up;
            case '<': return &spr::spr_stair_down;
            case 'T': return &spr::spr_trigger;
            case 's': return &spr::spr_npc_merchant;
            case 'n': {
                auto sn = spr::npcSprite().find(fl.npcEvent);
                return sn != spr::npcSprite().end() ? spr::byName(sn->second) : &spr::spr_npc_oldman;
            }
            default: return nullptr;
        }
    };

    // 地图
    for (int y = 0; y < fl.h; y++) {
        for (int x = 0; x < fl.w; x++) {
            int px = x * ui::CELL;
            int py = ui::MAP_Y + y * ui::CELL;
            ui::sprite(px, py, fl.isExplored(x, y) ? &spr::spr_floor : &spr::spr_floor_dark);
            const spr::Sprite* ent = tileSprite(x, y);
            if (ent) ui::sprite(px, py, ent);
            if (fl.markedWalls.count({x, y}))
                ui::overlayMark(px, py);
            if (markMode && markCursor.x == x && markCursor.y == y)
                ui::cursorBox(px, py);
        }
    }
    // 玩家在最上层
    ui::sprite(player.x * ui::CELL, ui::MAP_Y + player.y * ui::CELL, &spr::spr_hero);

    // 右侧列（col 13 起）文本行 1..9：属性面板
    int panelW = 34;
    std::vector<std::string> panelLines;
    {
        std::stringstream ss(drawPanelText(panelW));
        std::string line;
        while (std::getline(ss, line)) panelLines.push_back(line);
    }
    int nPanel = std::min((int)panelLines.size(), 9);
    for (int y = 0; y < nPanel; y++) {
        term::setpos(13, y + 1);
        term::print(panelLines[y]);
    }

    int logY = fl.h + 2;   // 地图下方文本区起始行（=11）
    // 日志：只显示最近 6 条（避免长循环后日志把预览/消息/提示挤出窗口）
    {
        int start = (int)log.size() > 6 ? (int)log.size() - 6 : 0;
        for (size_t k = (size_t)start; k < log.size(); k++) {
            term::setpos(0, logY - 1 + (int)(k - (size_t)start));
            term::print(term::dim() + "· " + term::reset() + log[k]);
        }
    }
    // 朝向怪物预览
    Point t = player + dirDelta(facing);
    auto fmit = fl.monsters.find(t);
    if (fmit != fl.monsters.end() && fmit->second.alive) {
        const MonsterDef* md = data.monster(fmit->second.id);
        if (md) {
            double aMul = 1.0, dMul = 1.0;
            isAuraBoosted(t, aMul, dMul);
            double atkMul = (loopCount >= 15) ? 1.1 : 1.0;
            bool heroFirst = (relicEffect() == "first_battle_firststrike" && battlesThisLoop == 0);
            BattleResult r = BattleSimulator::simulate(hero.st.hp, hero.st.atk, hero.st.def,
                                                       md->hp, md->atk, md->def, md->skills,
                                                       aMul, dMul, heroFirst, atkMul);
            std::string line = term::fg(1) + md->name + term::reset() + " 损血" + std::to_string(r.heroLossHp) +
                               " 约" + std::to_string(r.rounds) + "回合 ";
            line += r.heroWins ? (term::fg(2) + "可击败" + term::reset()) : (term::fg(1) + "会死" + term::reset());
            term::setpos(0, logY + 5);
            term::print(line);
        }
    }
    // 消息
    if (!message.empty()) {
        term::setpos(0, logY + 6);
        term::print(term::fg(5) + message + term::reset());
        message.clear();
    }
    // 提示
    term::setpos(0, logY + 8);
    term::print(term::dim() + "WASD移动 · Enter战斗 · P存档 · U突破 · H标记 · I图鉴 · Esc菜单" + term::reset());

    // 右侧目标文本框：标题 + 自动换行的完整目标文本（避免长目标被窗口右缘截断）
    {
        const int boxCol = 13;
        const int boxRow = logY - 1;                     // 10（与日志区同起点，右列并排）
        const int maxCols = 22;                          // 文本框行宽（col 13..34）
        const int maxRows = 5;                           // 正文最多 5 行
        std::vector<std::string> lines = term::wrapText(goalText(), maxCols);
        if ((int)lines.size() > maxRows) {
            lines.resize(maxRows);
            lines[maxRows - 1] += "…";
        }
        term::setpos(boxCol, boxRow);
        term::print(term::bold() + term::fg(3) + "▶ 目标" + term::reset());
        for (size_t k = 0; k < lines.size(); k++) {
            term::setpos(boxCol, boxRow + 1 + (int)k);
            term::print(lines[k]);
        }
    }
}

void Game::drawTile(int x, int y, std::string& out) const {
    const Floor& fl = floor();
    char c = fl.tile(x, y);
    // 标记模式光标
    if (markMode && markCursor.x == x && markCursor.y == y) {
        out += term::bg(6) + term::fg(0) + "*" + term::reset();
        return;
    }
    // 怪物
    auto mit = fl.monsters.find({x, y});
    if (mit != fl.monsters.end() && mit->second.alive) {
        const MonsterDef* md = data.monster(mit->second.id);
        if (md && md->boss) out += term::bold() + term::fg(1) + mit->second.ch + term::reset();
        else out += term::fg(1) + mit->second.ch + term::reset();
        return;
    }
    // 未拾取道具
    auto iit = fl.items.find({x, y});
    if (iit != fl.items.end() && !iit->second.taken) {
        out += term::fg(3) + c + term::reset();
        return;
    }
    if (c == '#' || c == 'H') {
        if (fl.markedWalls.count({x, y})) out += term::fg(4) + "*" + term::reset();
        else out += term::fg(7) + c + term::reset();
        return;
    }
    if (c == '1') { out += term::fg(3) + c + term::reset(); return; }
    if (c == '2') { out += term::fg(4) + c + term::reset(); return; }
    if (c == '3') { out += term::fg(1) + c + term::reset(); return; }
    if (c == '>' || c == '<') { out += term::fg(6) + c + term::reset(); return; }
    if (c == 'n') { out += term::bold() + term::fg(7) + c + term::reset(); return; }
    if (c == 's') { out += term::fg(5) + c + term::reset(); return; }
    if (c == 'T') { out += term::fg(3) + c + term::reset(); return; }
    if (c == '@') { out += term::bold() + term::fg(2) + c + term::reset(); return; }
    // 地板（未探索的变暗）
    if (!fl.isExplored(x, y)) { out += term::dim() + std::string("·") + term::reset(); return; }
    out += term::fg(7) + c + term::reset();
}

std::string Game::drawPanelText(int width) const {
    std::stringstream ss;
    auto line = [&](const std::string& s) { ss << term::pad(s, width) << "\n"; };
    line("");
    line("【勇者】");
    line("");
    line("生命 " + std::to_string(hero.st.hp));
    line("攻击 " + std::to_string(hero.st.atk) + " 防御 " + std::to_string(hero.st.def));
    line("金币 " + std::to_string(hero.st.gold) + " 经验 " + std::to_string(hero.st.exp));
    line("钥匙 黄" + std::to_string(hero.keys[0]) + " 蓝" + std::to_string(hero.keys[1]) + " 红" + std::to_string(hero.keys[2]));
    line("碎片 " + std::to_string(hero.fragments) + "/7  执念 " + std::to_string(obsession));
    {
        std::string relic = relicId.empty() ? "无" : (data.items.count(relicId) ? data.items.at(relicId).name : relicId);
        line("遗物 " + relic);
    }
    if (loopCount >= 15) line("※塔不耐烦：怪攻+10% 金币x2");
    if (loopCount >= 10) line("※部分怪物让路");
    line("");
    line("回数 " + std::to_string(loopCount) + " 层数 " + std::to_string(floorNumber()));
    return ss.str();
}

// ---------- 存档 ----------
std::string Game::saveName(int slot) const {
    if (savePath.empty()) return exeDir() + "\\save_" + std::to_string(slot) + ".dat";
    return savePath + "_" + std::to_string(slot) + ".dat";
}

// 时间戳 → "mm-dd hh:mm"（11 列，用于 14 列宽的槽位格子）
static std::string fmtSaveTs(uint64_t ts) {
    time_t t = (time_t)ts;
    struct tm tmv;
    localtime_s(&tmv, &t);
    char buf[32];
    strftime(buf, sizeof buf, "%m-%d %H:%M", &tmv);
    return buf;
}

// 楼层号 → 章号（1-5 第 1 章，6-10 第 2 章，11-20 第 3 章）
static std::string chapterOfFloor(int floorNo) {
    if (floorNo >= 20) return "第 3 章";
    if (floorNo >= 6) return "第 2 章";
    return "第 1 章";
}

bool Game::saveGame(int slot) {
    std::string path = saveName(slot);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    // 元数据头部（先于状态流；readSaveMeta 只读这段）
    f.write("RMOTASL8", 8);
    uint32_t ver = 2;                       // 流布局版本 2 = 含永久成长字段
    f.write((const char*)&ver, sizeof ver);
    uint64_t ts = (uint64_t)time(nullptr);
    f.write((const char*)&ts, sizeof ts);
    int32_t fl = floorNumber();
    f.write((const char*)&fl, sizeof fl);
    int32_t lp = loopCount;
    f.write((const char*)&lp, sizeof lp);
    int32_t fr = hero.fragments;
    f.write((const char*)&fr, sizeof fr);
    int32_t ob = obsession;
    f.write((const char*)&ob, sizeof ob);
    saveStream(f);
    f.flush();
    if (f) saveSlot = slot;
    return (bool)f;
}

bool Game::loadGame(int slot) {
    std::ifstream f(saveName(slot), std::ios::binary);
    if (!f) return false;
    char magic[9] = { 0 };
    f.read(magic, 8);
    std::string mg(magic, 8);
    bool hasPerm = true;
    if (mg == "RMOTASL8")      { hasPerm = true; }   // 当前版本（含永久成长）
    else if (mg == "RMOTASL7") { hasPerm = false; }  // 早期六槽版（无永久成长字段）
    else return false;
    uint32_t ver = 0;
    f.read((char*)&ver, sizeof ver);
    // 跳过元数据其余字段（readSaveMeta 负责展示）
    uint64_t ts; int32_t fl, lp, fr, ob;
    f.read((char*)&ts, sizeof ts);
    f.read((char*)&fl, sizeof fl);
    f.read((char*)&lp, sizeof lp);
    f.read((char*)&fr, sizeof fr);
    f.read((char*)&ob, sizeof ob);
    if (!loadStreamV(f, hasPerm)) return false;
    saveSlot = slot;
    return true;
}

bool Game::readSaveMeta(int slot, SaveMeta& out) const {
    out = SaveMeta();
    std::ifstream f(saveName(slot), std::ios::binary);
    if (!f) return false;
    char magic[9] = { 0 };
    f.read(magic, 8);
    std::string mg(magic, 8);
    if (mg != "RMOTASL8" && mg != "RMOTASL7") return false;
    uint32_t ver = 0;
    f.read((char*)&ver, sizeof ver);
    uint64_t ts; int32_t fl, lp, fr, ob;
    f.read((char*)&ts, sizeof ts);
    f.read((char*)&fl, sizeof fl);
    f.read((char*)&lp, sizeof lp);
    f.read((char*)&fr, sizeof fr);
    f.read((char*)&ob, sizeof ob);
    out.valid = !f.fail();
    out.ts = ts;
    out.floorNo = fl;
    out.loop = lp;
    out.fragments = fr;
    out.obsession = ob;
    return out.valid;
}

// ---------- 六格存档选择器 ----------
// 两行三列的六个框：每个框显示 进度/信息/日期。返回选中槽位 0-5；取消返回 -1。
void Game::drawSaveSlotFrame(bool forSave, int sel, const SaveMeta* metas, const std::string& tip) {
    // 每格：宽 14 列（col 0 / 15 / 31），高 5 行（row 3..7 / 9..13），窗口内正好放下
    const int colOf[3] = { 0, 15, 31 };
    term::clear();
    term::cursor(0);
    term::print(term::bold() + term::fg(3) + (forSave ? "— 保存到哪个存档位？ —" : "— 读取哪个存档？ —") + term::reset() + "\n\n");
    for (int i = 0; i < 6; i++) {
        int col = colOf[i % 3];
        int row = 3 + (i / 3) * 6;
        const SaveMeta& m = metas[i];
        bool hot = (i == sel);
        std::string frame = hot ? (term::bold() + term::fg(3)) : term::dim();
        std::string num = std::to_string(i + 1);
        std::string top = frame + "┌" + num + "───────────" + "┐" + term::reset();
        std::string bot = frame + "└────────────┘" + term::reset();
        // 内容三行（显示宽度 ≤12）
        std::string c1, c2, c3;
        if (m.valid) {
            c1 = chapterOfFloor(m.floorNo) + " " + std::to_string(m.floorNo) + "F";
            c2 = "轮回" + std::to_string(m.loop) + " 碎片" + std::to_string(m.fragments) + "/7";
            c3 = fmtSaveTs(m.ts);
        } else {
            c1 = "（空存档位）";
            c2 = "";
            c3 = "";
        }
        auto rowline = [&](const std::string& s) {
            return frame + "│" + term::pad(s, 12) + "│" + term::reset();
        };
        auto put = [&](int r, const std::string& s) {
            term::setpos(col, r);
            term::print(s);
        };
        put(row, top);
        put(row + 1, rowline(c1));
        put(row + 2, rowline(c2));
        put(row + 3, rowline(c3));
        put(row + 4, bot);
    }
    term::setpos(3, 15);
    if (!tip.empty()) {
        term::print(term::fg(5) + tip + term::reset());
    } else {
        term::print(term::dim() + "方向键/WASD 移动 · Enter 确认 · Esc 返回 · 数字 1-6 直选" + term::reset());
    }
}

int Game::saveSlotSelector(bool forSave) {
    const int SLOTS = 6;
    int sel = (saveSlot >= 0 && saveSlot < SLOTS) ? saveSlot : 0;
    SaveMeta metas[SLOTS];
    for (int i = 0; i < SLOTS; i++) readSaveMeta(i, metas[i]);
    std::string tip;   // 底部提示（如“该槽位没有存档”）
    for (;;) {
        // 整帧重画（present 会清空缓冲，无效键后必须重画）
        drawSaveSlotFrame(forSave, sel, metas, tip);
        int k = term::readKey();
        tip.clear();
        int dir = -1;
        if (k == 0x11) dir = 0;        // UP
        else if (k == 0x12) dir = 1;   // DOWN
        else if (k == 0x13) dir = 2;   // LEFT
        else if (k == 0x14) dir = 3;   // RIGHT
        else if (k == 'w' || k == 'W') dir = 0;
        else if (k == 's' || k == 'S') dir = 1;
        else if (k == 'a' || k == 'A') dir = 2;
        else if (k == 'd' || k == 'D') dir = 3;
        if (dir >= 0) {
            if (dir == 0) sel = (sel >= 3) ? sel - 3 : sel + 3;
            else if (dir == 1) sel = (sel < 3) ? sel + 3 : sel - 3;
            else if (dir == 2) sel = (sel % 3 == 0) ? sel + 2 : sel - 1;
            else sel = (sel % 3 == 2) ? sel - 2 : sel + 1;
            continue;
        }
        if (k >= '1' && k <= '6') {
            sel = k - '1';
            if (forSave || metas[sel].valid) return sel;
            tip = "该槽位没有存档。";
            continue;
        }
        if (k == 0x0D || k == ' ' || k == 'k' || k == 'K') {
            if (forSave || metas[sel].valid) return sel;
            tip = "该槽位没有存档。";
            continue;
        }
        if (k == 0x1B) return -1;
        // 其余按键忽略并重画
    }
}

void Game::saveStream(std::ostream& os) const {
    saveStreamV(os, true);
}

void Game::saveStreamV(std::ostream& os, bool hasPerm) const {
    auto wStr = [&](const std::string& s) {
        int n = (int)s.size();
        os.write((const char*)&n, sizeof n);
        os.write(s.data(), n);
    };
    os.write((const char*)&loopCount, sizeof loopCount);
    os.write((const char*)&obsession, sizeof obsession);
    // 英雄
    os.write((const char*)&hero.st, sizeof hero.st);
    os.write((const char*)hero.keys, sizeof hero.keys);
    os.write((const char*)&hero.fragments, sizeof hero.fragments);
    int fn = (int)hero.flags.size();
    os.write((const char*)&fn, sizeof fn);
    for (auto& f : hero.flags) wStr(f);
    wStr(relicId);
    os.write((const char*)&flagExtraKey, sizeof flagExtraKey);
    os.write((const char*)&battlesThisLoop, sizeof battlesThisLoop);
    os.write((const char*)shopCount, sizeof shopCount);
    os.write((const char*)&expShopFlags, sizeof expShopFlags);
    if (hasPerm) {
        os.write((const char*)&permAtk, sizeof permAtk);
        os.write((const char*)&permDef, sizeof permDef);
        os.write((const char*)&permHp, sizeof permHp);
    }
    wStr(cur);
    os.write((const char*)&player, sizeof player);
    os.write((const char*)&facing, sizeof facing);
    // 楼层
    int fn2 = (int)floorOrder.size();
    os.write((const char*)&fn2, sizeof fn2);
    for (auto& id : floorOrder) {
        wStr(id);
        auto it = floors.find(id);
        if (it != floors.end()) it->second.save(os);
        else { int dummy = -1; os.write((const char*)&dummy, sizeof dummy); }
    }
    // 图鉴
    int cn = (int)codex.size();
    os.write((const char*)&cn, sizeof cn);
    for (auto& kv : codex) {
        wStr(kv.first);
        os.write((const char*)&kv.second.seen, sizeof kv.second.seen);
        os.write((const char*)&kv.second.kills, sizeof kv.second.kills);
        os.write((const char*)&kv.second.minHpLoss, sizeof kv.second.minHpLoss);
    }
    // 结局
    int en = (int)unlockedEndings.size();
    os.write((const char*)&en, sizeof en);
    for (auto& e : unlockedEndings) wStr(e);
}

bool Game::loadStream(std::istream& is) {
    return loadStreamV(is, true);
}

bool Game::loadStreamV(std::istream& is, bool hasPerm) {
    auto rStr = [&]() {
        int n = 0;
        is.read((char*)&n, sizeof n);
        if (n < 0 || n > 100000) return std::string();
        std::string s;
        s.resize(n);
        if (n) is.read(&s[0], n);
        return s;
    };
    is.read((char*)&loopCount, sizeof loopCount);
    is.read((char*)&obsession, sizeof obsession);
    is.read((char*)&hero.st, sizeof hero.st);
    is.read((char*)hero.keys, sizeof hero.keys);
    is.read((char*)&hero.fragments, sizeof hero.fragments);
    int fn = 0;
    is.read((char*)&fn, sizeof fn);
    hero.flags.clear();
    for (int i = 0; i < fn; i++) hero.flags.push_back(rStr());
    relicId = rStr();
    is.read((char*)&flagExtraKey, sizeof flagExtraKey);
    is.read((char*)&battlesThisLoop, sizeof battlesThisLoop);
    is.read((char*)shopCount, sizeof shopCount);
    is.read((char*)&expShopFlags, sizeof expShopFlags);
    if (hasPerm) {
        is.read((char*)&permAtk, sizeof permAtk);
        is.read((char*)&permDef, sizeof permDef);
        is.read((char*)&permHp, sizeof permHp);
    } else {
        permAtk = permDef = permHp = 0;   // 旧版存档无永久成长（默认 0）
    }
    cur = rStr();
    is.read((char*)&player, sizeof player);
    is.read((char*)&facing, sizeof facing);
    // 楼层
    int fn2 = 0;
    is.read((char*)&fn2, sizeof fn2);
    floors.clear();
    for (int i = 0; i < fn2; i++) {
        std::string id = rStr();
        Floor f;
        f.load(is);
        floors[id] = std::move(f);
    }
    // 图鉴
    int cn = 0;
    is.read((char*)&cn, sizeof cn);
    codex.clear();
    for (int i = 0; i < cn; i++) {
        std::string id = rStr();
        CodexEntry e;
        is.read((char*)&e.seen, sizeof e.seen);
        is.read((char*)&e.kills, sizeof e.kills);
        is.read((char*)&e.minHpLoss, sizeof e.minHpLoss);
        codex[id] = e;
    }
    int en = 0;
    is.read((char*)&en, sizeof en);
    unlockedEndings.clear();
    for (int i = 0; i < en; i++) unlockedEndings.push_back(rStr());
    log.clear();
    // 容错：存档中的当前楼层不存在时回到 1F
    if (!floors.count(cur) && !floorOrder.empty()) cur = floorOrder[0];
    if (!floors.count(cur)) return false;
    return !is.fail();
}