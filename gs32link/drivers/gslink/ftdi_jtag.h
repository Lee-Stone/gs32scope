#ifndef GSLINK_FTDI_JTAG_H
#define GSLINK_FTDI_JTAG_H
/*
 * 层1: FTDI(FT2232H) D2XX + MPSSE 直驱, 对上提供 JTAG TAP 状态机。
 *
 * 关键设计(踩坑结论, 详见 debug.log):
 *  - 命令流用字节级命令压缩(整字节 0x19/0x39, 余位逐位 0x1B/0x3B, 末位 0x4B/0x6B)。
 *    USB 全速模式 64B 包上限会把长命令流分断, 但 JTAG 是静态逻辑, 分断处
 *    只是 TCK 停顿(哪怕停在移位中间), 不影响状态机正确性 — 早期"<64B 必须"
 *    的结论与 TMS bug 同期, 属混淆; 批量聚合模式正是利用这一点
 *  - Exit1→Update 迁移必须 TMS=1({0,...} 会卡死 Pause 态, IR/DR 不锁存)
 *  - IR 值缓存: 相同则跳过重扫(TAP 复位后缓存失效)
 *  - Update-DR 后插入 idleCycles_ 个 IDLE 周期(dtmcs.idle 时序, 由层2设置)
 */
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace gs32 {

typedef void* FT_HANDLE;
typedef unsigned long FT_STATUS;

/* D2XX 运行时动态加载(免安装期链接) */
struct D2xxApi {
    HMODULE mod = nullptr;
    FT_STATUS (WINAPI *Open)(int, FT_HANDLE*) = nullptr;
    FT_STATUS (WINAPI *OpenEx)(void*, unsigned long, FT_HANDLE*) = nullptr;
    FT_STATUS (WINAPI *ListDevices)(void*, void*, unsigned long) = nullptr;
    FT_STATUS (WINAPI *Close)(FT_HANDLE) = nullptr;
    FT_STATUS (WINAPI *Write)(FT_HANDLE, void*, unsigned long, unsigned long*) = nullptr;
    FT_STATUS (WINAPI *Read)(FT_HANDLE, void*, unsigned long, unsigned long*) = nullptr;
    FT_STATUS (WINAPI *SetBitMode)(FT_HANDLE, unsigned char, unsigned char) = nullptr;
    FT_STATUS (WINAPI *SetTimeouts)(FT_HANDLE, unsigned long, unsigned long) = nullptr;
    FT_STATUS (WINAPI *SetLatencyTimer)(FT_HANDLE, unsigned char) = nullptr;
    FT_STATUS (WINAPI *ResetDevice)(FT_HANDLE) = nullptr;
    FT_STATUS (WINAPI *Purge)(FT_HANDLE, unsigned long) = nullptr;

    bool load(std::string& err);
};

class FtdiJtag {
public:
    /* 打开指定描述的端口(port 为空/未找到时回退设备0)并完成 MPSSE 初始化 */
    bool open(uint32_t speed_khz, const char* port, std::string& err);
    /* 枚举 FTDI 设备描述(端口下拉用); false = D2XX 不可用 */
    bool listPorts(std::vector<std::string>& out);
    void close();
    bool isOpen() const { return ft_ != nullptr; }

    /* ---- TAP 状态机 ---- */
    bool tapReset();          /* Test-Logic-Reset -> IDLE, IR 缓存失效 */
    bool scanIR(uint8_t ir);  /* 设 IR(与缓存相同则跳过重扫) */
    bool scanIRn(int len, uint32_t ir);                     /* 指定长度的 IR 扫描 */
    bool scanDR(uint32_t bits, const uint8_t* tdi, uint8_t* tdo); /* DR 扫描, tdo 可空 */
    bool readDr32(uint32_t* v);   /* 32bit DR 读取便捷封装(tdi=0) */

    /* 多 TAP 扫描链: tapCount 总数, target 为目标序号(0 = 最靠近 TDO)。
     * 非目标 TAP 在 IR 扫描时置 BYPASS(0x1F), DR 扫描插入 1bit 0。 */
    void setChain(int tapCount, int target);
    int  tapCount() const { return tapCount_; }
    int  targetTap() const { return targetTap_; }

    /* Update-DR 之后保持的 IDLE 周期数(dtmcs.idle, 由层2 在读 DTMCS 后设置) */
    void setIdleCycles(int n) { idleCycles_ = n; }

    /* ---- 诊断 ---- */
    int  irLength() const { return irLen_; }
    int  probeIrLength(); /* IR 长度探测(特征串回读) */
    void readBackIR();    /* 回读当前 IR(Capture-IR) */

    /* ---- 批量聚合模式 ----
     * 聚合多次 scanDR 为单笔 FT_Write + 单笔 FT_Read, 消除事务间的
     * USB 往返间隔(实测包间隔 100µs~1ms, 是采样吞吐瓶颈)。
     * 命令流超过 4096B 自动分段; tdo 指针须保持有效直到 flush。 */
    void beginBatch();
    bool flushBatch();

    /* ---- 插桩统计(自上次 takeLastWriteMs 清零) ----
     * lastWriteMs: 最近一次 flushBatch 的 FT_Write 耗时(ms) */
    double lastWriteMs() const { return lastWriteMs_; }
    void   resetLastWriteMs() { lastWriteMs_ = 0.0; }

private:
    bool writeFull_(const void* buf, uint32_t n);
    bool readFull_(void* buf, uint32_t n);
    static void appendTms_(std::vector<uint8_t>& cmd, uint8_t tmsBits, size_t n,
                           bool tdiHigh);
    bool tmsShift_(const std::vector<bool>& tms, bool tdi);
    bool tmsShiftLast_(bool tdi, uint8_t* tdoBit);
    bool dataShift_(const uint8_t* tdiBytes, uint32_t bits, uint8_t* tdoOut);
    bool scanDRRaw(uint32_t bits, const uint8_t* tdi, uint8_t* tdo);

    D2xxApi api_;
    FT_HANDLE ft_ = nullptr;
    int irLen_ = 5;        /* IR 长度(probe 可修正) */
    int tapCount_ = 1;     /* JTAG 链 TAP 总数 */
    int targetTap_ = 0;    /* 目标 TAP 序号, 0 = 最靠近 TDO */
    int curIr_ = -1;       /* 当前 IR 缓存, 相同则跳过重扫 */
    int idleCycles_ = 0;   /* Update-DR 后 IDLE 周期数 */

    /* 批量聚合状态。
     * kBatchMax: 单次 FT_Write 命令流上限。4096 = FT2232H 内部缓冲大小(实测稳定);
     * 64KB 曾出现一次首字虚假 resp=3(疑硬件/链路偶发, 待复测确认)。 */
    struct BatchScan { uint32_t bits; uint8_t* tdo; uint32_t rxB; };
    static constexpr size_t kBatchMax = 64 * 1024;
    bool batching_ = false;
    std::vector<uint8_t> batchCmd_;
    std::vector<BatchScan> batchScans_;
    double lastWriteMs_ = 0.0;   /* 最近一次 flushBatch 的 FT_Write 耗时 */
};

} // namespace gs32

#endif /* GSLINK_FTDI_JTAG_H */
