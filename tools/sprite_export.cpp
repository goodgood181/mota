// sprite_export.cpp —— 把 sprites.h 的精灵导出为 BMP（放大 8 倍，透明=品红）
// 用法: sprite_export.exe <outdir>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../src/sprites.h"

static void writeBmp(const std::string& path, int w, int h, const std::vector<uint32_t>& px) {
    int rowBytes = (w * 3 + 3) & ~3;
    int imgBytes = rowBytes * h;
    std::vector<uint8_t> file(54 + imgBytes, 0);
    file[0] = 'B'; file[1] = 'M';
    uint32_t size = (uint32_t)file.size();
    memcpy(&file[2], &size, 4);
    uint32_t off = 54;
    memcpy(&file[10], &off, 4);
    uint32_t header = 40;
    memcpy(&file[14], &header, 4);
    int32_t w32 = w, h32 = h;
    memcpy(&file[18], &w32, 4);
    memcpy(&file[22], &h32, 4);
    uint16_t planes = 1; memcpy(&file[26], &planes, 2);
    uint16_t bpp = 24;  memcpy(&file[28], &bpp, 2);
    for (int y = 0; y < h; y++) {
        int row = h - 1 - y; // 位图自底向上
        for (int x = 0; x < w; x++) {
            uint32_t c = px[y * w + x];
            uint8_t* p = &file[54 + row * rowBytes + x * 3];
            p[0] = c & 0xFF; p[1] = (c >> 8) & 0xFF; p[2] = (c >> 16) & 0xFF;
        }
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (f) { fwrite(file.data(), 1, file.size(), f); fclose(f); }
    (void)imgBytes;
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "build/sprites";
    int zoom = 8;
    // 收集全部精灵
    struct Entry { const char* name; const spr::Sprite* s; };
    std::vector<Entry> entries;
    {
        using namespace spr;
        entries = {
            { "floor", &spr_floor }, { "floor_dark", &spr_floor_dark },
            { "wall", &spr_wall }, { "wall_h", &spr_wall_h },
            { "door1", &spr_door1 }, { "door2", &spr_door2 }, { "door3", &spr_door3 },
            { "stair_up", &spr_stair_up }, { "stair_down", &spr_stair_down },
            { "trigger", &spr_trigger }, { "mark", &spr_mark },
            { "hero", &spr_hero }, { "npc_oldman", &spr_npc_oldman },
            { "npc_merchant", &spr_npc_merchant }, { "npc_shade", &spr_npc_shade },
            { "npc_guide", &spr_npc_guide },
            { "slime_green", &spr_slime_green }, { "slime_split", &spr_slime_split },
            { "slime_blue", &spr_slime_blue }, { "bat", &spr_bat },
            { "skeleton", &spr_skeleton }, { "wraith", &spr_wraith },
            { "vampire", &spr_vampire }, { "golem", &spr_golem }, { "guard", &spr_guard },
            { "iron_soldier", &spr_iron_soldier }, { "stone_guard", &spr_stone_guard },
            { "dark_mage", &spr_dark_mage }, { "mirror_guard", &spr_mirror_guard },
            { "shadow_elf", &spr_shadow_elf }, { "banshee", &spr_banshee },
            { "black_knight", &spr_black_knight }, { "heart_demon", &spr_heart_demon },
            { "key_y", &spr_key_y }, { "key_b", &spr_key_b }, { "key_r", &spr_key_r },
            { "hp_small", &spr_hp_small }, { "hp_big", &spr_hp_big },
            { "atk", &spr_atk }, { "def", &spr_def }, { "gold", &spr_gold },
            { "fragment", &spr_fragment }, { "relic", &spr_relic },
        };
    }
    for (auto& e : entries) {
        // 数据校验：每行必须 16 字符，字符必须在调色板中
        bool rowOk = true;
        for (int sy = 0; sy < 16; sy++) {
            const std::string& r = e.s->rows[sy];
            if ((int)r.size() != 16) { printf("[FAIL] %s 行%d 长度=%d\n", e.name, sy, (int)r.size()); rowOk = false; }
            for (char c : r)
                if (c != '.' && !spr::palette().count(c)) {
                    printf("[FAIL] %s 行%d 非法字符 '%c'\n", e.name, sy, c);
                    rowOk = false;
                }
        }
        if (!rowOk) { printf("      跳过 %s\n", e.name); continue; }

        int w = 16 * zoom, h = 16 * zoom;
        std::vector<uint32_t> px(w * h, 0xFF00FFu); // 品红=透明
        for (int sy = 0; sy < 16; sy++)
            for (int sx = 0; sx < 16; sx++) {
                char c = e.s->rows[sy][sx];
                auto it = spr::palette().find(c);
                if (it == spr::palette().end()) continue;
                uint32_t col = 0xFF000000u | (it->second.r << 16) | (it->second.g << 8) | it->second.b;
                for (int dy = 0; dy < zoom; dy++)
                    for (int dx = 0; dx < zoom; dx++)
                        px[(sy * zoom + dy) * w + (sx * zoom + dx)] = col;
            }
        std::string path = dir + "\\" + e.name + ".bmp";
        writeBmp(path, w, h, px);
        printf("ok %s\n", e.name);
    }
    printf("done -> %s\n", dir.c_str());
    return 0;
}