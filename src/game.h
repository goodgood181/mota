// game.h —— 游戏主状态与逻辑（声明）
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include <deque>
#include <cstdint>
#include <ostream>
#include <istream>
#include "data.h"
#include "events.h"
#include "floorgame.h"
#include "types.h"

// ---------- 图鉴条目 ----------
struct CodexEntry {
    std::string id;
    bool seen = false;   // 首次遭遇
    int kills = 0;       // 击杀数
    int minHpLoss = -1;  // 最佳（最小）损血记录
};

// ---------- 勇者 ----------
struct Hero {
    Stats st;
    int keys[3] = {0, 0, 0};
    int fragments = 0;
    std::vector<std::string> flags;

    bool hasFlag(const std::string& f) const {
        for (auto& s : flags) if (s == f) return true;
        return false;
    }
    void setFlag(const std::string& f) {
        if (!hasFlag(f)) flags.push_back(f);
    }
};

// ---------- 主游戏 ----------
class Game {
public:
    GameData data;
    std::map<std::string, DialogScene> scenes;
    std::vector<std::string> floorOrder;   // 楼层 id 顺序

    // ===== 持久状态（跨轮回/跨存档） =====
    int loopCount = 0;
    int obsession = 0;
    std::map<std::string, CodexEntry> codex;
    std::vector<std::string> unlockedEndings;

    // ===== 本轮状态 =====
    Hero hero;
    std::map<std::string, Floor> floors;
    std::string cur;              // 当前楼层 id
    Point player;                 // 当前玩家位置
    int facing = 3;               // 朝向 0上1下2左3右
    std::string relicId;          // 本轮回携带的遗物
    bool flagExtraKey = false;    // 执念：每层多1把黄钥匙
    int battlesThisLoop = 0;      // 本轮已战斗次数（先手遗物用）
    int shopCount[3] = {0,0,0};   // 金币店购买次数（价格递增）
    int expShopFlags = 0;         // 经验店已购次数
    int permAtk = 0, permDef = 0, permHp = 0; // 经验突破的永久成长（跨轮回保留）
    std::set<std::string> extraKeyGranted; // 本轮已发过额外钥匙的楼层

    bool running = true;
    std::deque<std::string> log;
    std::string message;          // 次要面板消息
    bool markMode = false;        // 暗墙标记模式
    Point markCursor{0, 0};
    bool suppressScenes = false;  // 无终端环境（自检）时跳过对话
    std::string savePath;         // 存档前缀目录+基名（save_<n>.dat 由 saveName 拼接）
    int saveSlot = -1;            // 当前使用中的存档槽位（0-5；-1 = 未绑定/新游戏）

    // ---------- 生命周期 ----------
    bool init(const std::string& dataDir, std::string& err);
    int floorIndex() const;
    Floor& floor();
    const Floor& floor() const;
    int floorNumber() const;      // 1-based

    void newGame();               // 全新开始
    void resetForLoop();          // 轮回重置（保留 图鉴/探索/碎片/执念/轮回次数）
    std::string relicEffect() const; // 当前遗物的效果键（items.json relic_effect）

    // ---------- 输入与移动 ----------
    void handleKey(int key);
    void tryMove(int dir);
    void checkStep(const Point& t);
    void changeFloor(bool up);
    void openShop();
    void shopBuy(int kind);
    void shopExpMenu();
    void talkNpc();
    void talkNpcAt(const Point& np);
    void triggerStep();
    void pickupItem(const Point& p);
    void markWallMode();          // H 标记暗墙
    void showCodex(bool inGame);
    void pauseMenu();

    // ---------- 战斗 ----------
    struct BattleOutcome {
        bool won = false;
        int lossHp = 0;
        int rounds = 0;
        int curseAtk = 0;
        bool firstStrike = false;
    };
    bool battleAt(const Point& pos, bool confirm);
    void applyBattleWin(const std::string& mid, const Point& pos, const BattleOutcome& o);
    bool isAuraBoosted(const Point& pos, double& atkMul, double& defMul) const;

    // ---------- 事件 ----------
    CondContext makeCtx() const;
    void runScene(const std::string& id);

    // ---------- 轮回与结局 ----------
    void die(const std::string& reason);
    void deathScreen();
    bool chooseRelic();           // 返回是否选定了遗物（选了则结束结算、直接开启新轮回）
    void checkEndings(bool atTop);

    // ---------- 渲染 ----------
    void render();
    void renderGui();   // 图形模式整帧
    void drawTile(int x, int y, std::string& out) const;
    std::string drawPanelText(int width) const;
    void addLog(const std::string& s);
    std::string goalText() const; // 当前主线指引（数据 goal + 动态补充）

    // ---------- 商店常量 ----------
    static const int SHOP_GOLD_ATK = 0, SHOP_GOLD_DEF = 1, SHOP_GOLD_HP = 2;
    int shopPrice(int kind) const;

    // ---------- 存档 ----------
    // 六个存档槽位（save_0.dat .. save_5.dat），含元数据（时间/楼层/轮回/碎片/执念）
    struct SaveMeta {
        bool valid = false;       // 该槽位是否有档
        uint64_t ts = 0;          // 存档时间（UNIX 秒）
        int floorNo = 1;          // 当前楼层号（1-based）
        int loop = 0;             // 轮回次数
        int fragments = 0;        // 碎片收集数
        int obsession = 0;        // 执念数
    };
    std::string saveName(int slot) const;
    bool saveGame(int slot);
    bool loadGame(int slot);
    bool readSaveMeta(int slot, SaveMeta& out) const; // 只读头部元数据（供槽位界面显示）
    void drawSaveSlotFrame(bool forSave, int sel, const SaveMeta* metas, const std::string& tip);
    int saveSlotSelector(bool forSave);               // 两行三列槽位选择器；返回 0-5 或 -1（取消）
    void saveStream(std::ostream& os) const;   // 当前布局（含永久成长字段）
    void saveStreamV(std::ostream& os, bool hasPerm) const; // 版本化布局（旧档兼容用）
    bool loadStream(std::istream& is);
    bool loadStreamV(std::istream& is, bool hasPerm);       // hasPerm=false = RMOTASL7 时代布局
};