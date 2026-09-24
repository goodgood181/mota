// battle.h —— 战斗模拟器（纯函数，只模拟不改状态）
// 经典魔塔公式：
//   玩家伤害 = max(1, 玩家攻击 − 怪物防御)
//   回合数   = ceil(怪物生命 / 玩家伤害)
//   玩家损血 = (回合数 − 1) × max(0, 怪物攻击 − 玩家防御)
// 技能：先攻 / 连击 / 吸血 / 诅咒 / 分裂 / 领域（领域在调用侧改属性）
#pragma once
#include <string>
#include <vector>
#include <cmath>
#include "types.h"

enum Skill {
    SKILL_NONE = 0,
    SKILL_FIRST_STRIKE, // 先攻：战斗前白打一下
    SKILL_COMBO,        // 连击：每回合攻击2次
    SKILL_LIFESTEAL,    // 吸血：每回合回复自身攻击50%的血
    SKILL_CURSE,        // 诅咒：战后永久 −X 攻
    SKILL_SPLIT,        // 分裂：死亡后变成2只小的
    SKILL_AURA          // 领域：相邻4格怪物攻防+30%
};

struct BattleResult {
    bool heroWins = false;
    int heroLossHp = 0;      // 战斗全程玩家损血（先攻的那一下也算）
    int rounds = 0;          // 玩家出手回合数
    int heroDmgPerRound = 1; // 玩家每回合伤害
    int monDmgPerRound = 0;  // 怪物每回合对玩家的伤害（含连击翻倍）
    bool bossFirstStrike = false; // 是否触发了先攻
    int curseAtkLoss = 0;    // 诅咒造成的攻击损失（仅胜利时生效）
};

class BattleSimulator {
public:
    // hero: 玩家 生命/攻击/防御
    // mon:  怪物 生命/攻击/防御
    // skills: 怪物技能字符串
    // auraAtkMul / auraDefMul: 领域加成（调用侧算好传入）
    // heroFirst: 玩家先手（遗物「赤铠之怒」等）
    // atkMul: 全局怪物攻击倍率（轮回15+的"不耐烦"）
    static BattleResult simulate(int hHp, int hAtk, int hDef,
                                 int mHp, int mAtk, int mDef,
                                 const std::vector<std::string>& skills,
                                 double auraAtkMul = 1.0, double auraDefMul = 1.0,
                                 bool heroFirst = false, double atkMul = 1.0) {
        BattleResult r;
        int mAtkF = (int)std::lround(mAtk * auraAtkMul);
        int mDefF = (int)std::lround(mDef * auraDefMul);
        int mAtkFinal = (int)std::lround(mAtkF * atkMul);

        bool firstStrike = hasSkill(skills, SKILL_FIRST_STRIKE);
        bool combo = hasSkill(skills, SKILL_COMBO);
        bool lifesteal = hasSkill(skills, SKILL_LIFESTEAL);

        r.heroDmgPerRound = std::max(1, hAtk - mDefF);
        r.monDmgPerRound = std::max(0, mAtkFinal - hDef);
        if (combo) r.monDmgPerRound *= 2;

        int loss = 0;
        int mhp = mHp;
        int healCap = mHp;

        // 先攻：开战前白打一下（如果怪物先手）
        if (firstStrike && !heroFirst) {
            int dmg = std::max(0, mAtkFinal - hDef);
            loss += dmg;
            r.bossFirstStrike = true;
        }

        bool heroDead = false;
        // 经典节奏：玩家先出手，怪物若存活则反击
        while (mhp > 0) {
            // 玩家反击/先手
            mhp -= r.heroDmgPerRound;
            r.rounds++;
            if (mhp <= 0) break; // 怪物死亡，无反击
            // 怪物反击
            loss += r.monDmgPerRound;
            if (loss >= hHp) { heroDead = true; break; }
            // 吸血：每回合回复自身攻击50%
            if (lifesteal && mAtkFinal > 0) {
                int heal = mAtkFinal / 2;
                if (heal < 1) heal = 1;
                mhp += heal;
                if (mhp > healCap) mhp = healCap;
            }
        }

        r.heroLossHp = loss;
        r.heroWins = !heroDead;

        // 诅咒：胜利后永久扣攻
        if (r.heroWins && hasSkill(skills, SKILL_CURSE)) {
            r.curseAtkLoss = 5; // 默认 -5 攻，可被怪物数据覆盖
        }
        return r;
    }

    // 解析技能字符串
    static Skill parse(const std::string& s) {
        if (s == "first_strike") return SKILL_FIRST_STRIKE;
        if (s == "combo")        return SKILL_COMBO;
        if (s == "lifesteal")    return SKILL_LIFESTEAL;
        if (s == "curse")        return SKILL_CURSE;
        if (s == "split")        return SKILL_SPLIT;
        if (s == "aura")         return SKILL_AURA;
        return SKILL_NONE;
    }
    static bool hasSkill(const std::vector<std::string>& skills, Skill s) {
        for (auto& k : skills) if (parse(k) == s) return true;
        return false;
    }
    // 技能中文名（用于图鉴/预览）
    static const char* cnName(Skill s) {
        switch (s) {
            case SKILL_FIRST_STRIKE: return "先攻";
            case SKILL_COMBO:        return "连击";
            case SKILL_LIFESTEAL:    return "吸血";
            case SKILL_CURSE:        return "诅咒";
            case SKILL_SPLIT:        return "分裂";
            case SKILL_AURA:         return "领域";
            default:                 return "";
        }
    }
};