#ifndef CHIP_PARTINFO_H
#define CHIP_PARTINFO_H
/*
 * 目标芯片部件号签名 — 用于连接时做精确型号校验。
 *
 * 数据来源: OpenOCD 的 gs32_model.c 期望表(2026-11-13 逐型号采集)。
 * 运行期从芯片 OTP 信息块 0x1FFF40D0 读 8 字节:
 *   [2] = Flash 档位, [3..4] = SYS_Freq(MHz, 小端 16bit), [6..7] = DEV_Seq(大端 16bit)
 *
 * 注意: 同一 (DEV_Seq, Freq, Flash) 的多个型号是同 die 不同封装/温度等级,
 * 硬件上无法区分, 校验时视为等价。表里没有的型号不做精确校验。
 */
#include <cstdint>
#include <cstring>

struct ChipPartInfo {
    const char* name;
    uint16_t devSeq;
    uint16_t freqMHz;
    uint8_t flashIdx;
};

inline constexpr ChipPartInfo kChipPartInfo[] = {
    {"GS32F0039", 0x0039, 240, 5},
    {"GS32F0039H", 0x0039, 300, 5},
    {"GS32F0039H1", 0x0039, 240, 5},
    {"GS32F0039P", 0x0039, 240, 6},
    {"GS32F0038-Q", 0x0038, 240, 4},
    {"GS32F0039-Q", 0x0039, 240, 5},
    {"GS32F0039H-Q", 0x0039, 240, 6},
    {"GS32F0039A", 0x0039, 240, 4},
    {"GS32F0049", 0x0049, 200, 4},
    {"GS32F0049H", 0x0049, 240, 5},
    {"GS32F0049H1", 0x0049, 200, 5},
    {"GS32F0049A", 0x0049, 240, 4},
    {"GS32F0025A", 0x0025, 160, 2},
    {"GS32FMT5000A", 0x5000, 320, 4},
    {"GS32FMT5800", 0x5800, 400, 6},
    {"GS32F0025", 0x0025, 160, 2},
    {"GS32F0025H", 0x0025, 240, 4},
    {"GS32F035", 0xF035, 180, 2},
    {"GS32F035H", 0xF035, 180, 2},
    {"GS32F035P", 0xF035, 240, 4},
    {"GS32F00137", 0xC137, 180, 3},
    {"GS32F00137H", 0xC137, 240, 4},
    {"GS32F00157", 0xC157, 180, 4},
    {"GS32F00157H", 0xC157, 240, 4},
    {"GS32F00157-Q", 0xC157, 180, 4},
    {"GS32FP55A", 0xD550, 300, 4},
    {"GS32FMT4000", 0x4000, 160, 4},
    {"GS32FMT5000", 0x5000, 400, 4},
    {"GS32FPW3300", 0x3300, 120, 4},
    {"GS32FPW3600", 0x3600, 240, 4},
    {"GS32F075", 0xF075, 240, 6},
    {"GS32F075D", 0xF075, 160, 6},
    {"GS32F374S", 0xF374, 300, 6},
    {"GS32F374SX", 0xF374, 300, 6},
    {"GS32F374SH", 0xF374, 400, 6},
    {"GS32F374SH1", 0xF374, 300, 6},
    {"GS32F374D", 0xF374, 300, 6},
    {"GS32F374DX", 0xF374, 300, 6},
    {"GS32F374DH", 0xF374, 400, 6},
    {"GS32F374DH1", 0xF374, 300, 6},
    {"GS32F377S", 0xF377, 300, 8},
    {"GS32F377SX", 0xF377, 300, 8},
    {"GS32F377SH", 0xF377, 400, 8},
    {"GS32F377SH1", 0xF377, 300, 8},
    {"GS32F377D", 0xF377, 300, 8},
    {"GS32F377DH", 0xF377, 400, 8},
    {"GS32F377DH1", 0xF377, 300, 8},
    {"GS32F379S", 0xF379, 300, 8},
    {"GS32F379SX", 0xF379, 300, 8},
    {"GS32F379SH", 0xF379, 400, 8},
    {"GS32F379SH1", 0xF379, 300, 8},
    {"GS32F379D", 0xF379, 300, 8},
    {"GS32F379DY", 0xC379, 300, 8},
    {"GS32F379DX", 0xF379, 300, 8},
    {"GS32F379DH", 0xF379, 400, 8},
    {"GS32F379DH1", 0xF379, 300, 8},
    {"GS32F388DA", 0xF388, 300, 8},
    {"GS32F388SX", 0xF388, 300, 8},
    {"GS32FP65SK", 0xD650, 300, 8},
    {"GS32FP65SKX", 0xD650, 300, 8},
    {"GS32FP65SKH", 0xD650, 400, 8},
    {"GS32FP65SKH1", 0xD650, 300, 8},
    {"GS32FP65DK", 0xD650, 300, 8},
    {"GS32FP65DKX", 0xD650, 300, 8},
    {"GS32FP65DKH", 0xD650, 400, 8},
    {"GS32FP65DKH1", 0xD650, 300, 8},
};

inline const ChipPartInfo* findChipPartInfo(const char* name) {
    if (!name) return nullptr;
    for (const auto& e : kChipPartInfo)
        if (std::strcmp(e.name, name) == 0) return &e;
    return nullptr;
}

#endif /* CHIP_PARTINFO_H */
