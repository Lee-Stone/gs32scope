#ifndef CHIP_MODELS_H
#define CHIP_MODELS_H
/*
 * 目标型号表 - 与 GS32Prog V1.1.5 的 CHIPMODELS.get_all_models() 一致。
 * series: 该型号所属的 OpenOCD 配置族 (cs12/cs30/cs22/cs23)。
 * idcode: 该配置族 JTAG IDCODE; 0 表示 GS32Prog 未提供配置(当前不支持)。
 * taps:   JTAG 链上的 TAP 总数; tap: 目标 TAP 序号(0 = 最靠近 TDO)。
 *         cs23 (37xA) 为 cpu2+cpu1 双 TAP, 目标是第二个 cpu1。
 * 连接时由驱动读取实际 IDCODE 并校验, 再按 taps/tap 组装扫描链。
 */
#include <cstdint>
#include <cstring>

struct ChipModelDef {
    const char* name;
    const char* series;
    uint32_t idcode;
    uint8_t taps;
    uint8_t tap;
};

inline constexpr ChipModelDef kChipModels[] = {
    {"GS32F0039", "cs12", 0x1030AA6Du, 1, 0},
    {"GS32F0039H", "cs12", 0x1030AA6Du, 1, 0},
    {"GS32F0039H1", "cs12", 0x1030AA6Du, 1, 0},
    {"GS32F0039P", "cs12", 0x1030AA6Du, 1, 0},
    {"GS32F0038-Q", "cs12", 0x1030AA6Du, 1, 0},
    {"GS32F0039-Q", "cs12", 0x1030AA6Du, 1, 0},
    {"GS32F0039H-Q", "cs12", 0x1030AA6Du, 1, 0},
    {"GS32F0039A", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32F0049", "cs12", 0x1030AA6Du, 1, 0},
    {"GS32F0049H", "cs12", 0x1030AA6Du, 1, 0},
    {"GS32F0049H1", "cs12", 0x1030AA6Du, 1, 0},
    {"GS32F0049A", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32F0025A", "cs12", 0x1030AA6Du, 1, 0},
    {"GS32FMT5000A", "cs12", 0x1030AA6Du, 1, 0},
    {"GS32FMT5800", "cs12", 0x1030AA6Du, 1, 0},
    {"GS32F0025", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32F0025H", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32F035", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32F035H", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32F035P", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32F00137", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32F00137H", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32F00157", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32F00157H", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32F00157-Q", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32FP55A", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32FMT4000", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32FMT5000", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32FPW3300", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32FPW3600", "cs30", 0x1030AA6Du, 1, 0},
    {"GS32F075", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F075D", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F374S", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F374SX", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F374SH", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F374SH1", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F374D", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F374DX", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F374DH", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F374DH1", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F377S", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F377SX", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F377SH", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F377SH1", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F377D", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F377DX", "", 0u, 0, 0},
    {"GS32F377DH", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F377DH1", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F379S", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F379SX", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F379SH", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F379SH1", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F379D", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F379DY", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F379DX", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F379DH", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F379DH1", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F388DA", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F388SX", "cs22", 0x10900A6Du, 1, 0},
    {"GS32FP65SK", "cs22", 0x10900A6Du, 1, 0},
    {"GS32FP65SKX", "cs22", 0x10900A6Du, 1, 0},
    {"GS32FP65SKH", "cs22", 0x10900A6Du, 1, 0},
    {"GS32FP65SKH1", "cs22", 0x10900A6Du, 1, 0},
    {"GS32FP65DK", "cs22", 0x10900A6Du, 1, 0},
    {"GS32FP65DKX", "cs22", 0x10900A6Du, 1, 0},
    {"GS32FP65DKH", "cs22", 0x10900A6Du, 1, 0},
    {"GS32FP65DKH1", "cs22", 0x10900A6Du, 1, 0},
    {"GS32F379A", "", 0u, 0, 0},
    {"GS32F377SA", "cs23", 0x60900A6Du, 2, 1},
    {"GS32F374SA", "cs23", 0x60900A6Du, 2, 1},
    {"GS32F379SHA", "cs23", 0x60900A6Du, 2, 1},
    {"GS32F377SHA", "cs23", 0x60900A6Du, 2, 1},
    {"GS32F374SHA", "cs23", 0x60900A6Du, 2, 1},
    {"GS32F379DA", "cs23", 0x60900A6Du, 2, 1},
    {"GS32F377DA", "cs23", 0x60900A6Du, 2, 1},
    {"GS32F374DA", "cs23", 0x60900A6Du, 2, 1},
    {"GS32F379DHA", "cs23", 0x60900A6Du, 2, 1},
    {"GS32F377DHA", "cs23", 0x60900A6Du, 2, 1},
    {"GS32F374DHA", "cs23", 0x60900A6Du, 2, 1},
    {"GS32F388S", "", 0u, 0, 0},
    {"GS32F388SH", "", 0u, 0, 0},
    {"GS32F388D", "", 0u, 0, 0},
    {"GS32F388DH", "", 0u, 0, 0},
    {"GS32FP65SKE", "cs23", 0x60900A6Du, 2, 1},
    {"GS32FP65SKEH", "cs23", 0x60900A6Du, 2, 1},
    {"GS32FP65DKE", "cs23", 0x60900A6Du, 2, 1},
    {"GS32FP65DKEH", "cs23", 0x60900A6Du, 2, 1},
};

inline constexpr int kChipModelCount =
    int(sizeof(kChipModels) / sizeof(kChipModels[0]));

inline const ChipModelDef* findChipModel(const char* name) {
    if (!name) return nullptr;
    for (int i = 0; i < kChipModelCount; ++i)
        if (std::strcmp(kChipModels[i].name, name) == 0)
            return &kChipModels[i];
    return nullptr;
}

#endif /* CHIP_MODELS_H */
