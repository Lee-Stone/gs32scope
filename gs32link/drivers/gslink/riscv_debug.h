#ifndef GSLINK_RISCV_DEBUG_H
#define GSLINK_RISCV_DEBUG_H
/*
 * 层2: RISC-V Debug Spec 0.13.2 — DTM(IR=0x10/0x11) 与 DM 寄存器操作,
 * 经 SBA(System Bus Access) 读写目标内存(不暂停内核)。
 * 对上提供: ensureReady() 初始化 + readMem/writeMem 内存读写。
 *
 * 关键设计(踩坑结论, 详见 debug.log):
 *  - DMI 帧 41bit {op[1:0], data[31:0], addr[6:0]} LSB 先出
 *  - SBDATA0 = 0x3C(0x3D 是 sbdata1 高 32 位, 32 位目标恒 0 — 最大踩坑)
 *  - SBCS W1C 清错必须独立一笔写(混在配置位里清不掉)
 *  - 读路径无需轮询 sbbusy(JTAG ms 级 >> 总线 ns 级); 写路径必须等
 *  - DM 激活判定: 写 dmactive 后回读 dmcontrol(dmstatus 无此位)
 */
#include "ftdi_jtag.h"
#include "gs32link_util.h"

namespace gs32 {

class RiscvDebug {
public:
    RiscvDebug(FtdiJtag& jtag, ErrorState& err) : j_(jtag), err_(err) {}

    /* DTM/DM 初始化(幂等): TAP复位/IDCODE诊断/DTM复位/DM激活/SBA配置。
     * 非线程安全, 由上层加锁后调用 */
    int  ensureReady();
    bool isReady() const { return ready_; }

    /* 内存读写(8/32bit SBA, 头尾按 8bit 对齐补偿) */
    int  readMem (uint32_t addr, uint8_t* dst, uint32_t size);
    int  writeMem(uint32_t addr, const uint8_t* src, uint32_t size);

    /* 多区域批量读取: 全部区域的扫描(含宽度切换/头尾 8bit 补偿)聚合为
     * 单笔 USB 事务 — 多变量采样时消除变量间的 ftd2xx 往返间隔。
     * 出参布局: 各区域数据按 regions 顺序紧密相连 */
    struct MemRegion { uint32_t addr; uint32_t size; };
    int  readRegions(const MemRegion* regs, uint32_t count, uint8_t* out);

    /* 诊断探测(只读, 不改变就绪状态) */
    void sbaDiag(uint32_t probe_addr);       /* SBA 两条读路径对比 */
    void abstractDiag(uint32_t probe_addr);  /* Abstract Command 路径 */

    /* 插桩统计: 透传层1 最近一次 flushBatch 的 FT_Write 耗时(ms) */
    double lastWriteMs() const { return j_.lastWriteMs(); }
    void   resetLastWriteMs() { j_.resetLastWriteMs(); }

private:
    int dmiAccess(uint8_t op, uint32_t addr, uint32_t data, uint32_t* outData);
    int dmiWriteReg(uint32_t addr, uint32_t data);
    int dmiReadReg(uint32_t addr, uint32_t* data);
    uint32_t sbcsCfg_(uint8_t width) const;
    int setSbAccess(uint8_t width);
    int sbaConfigure();
    int sbWaitIdle();
    int sbaReadN(uint32_t addr, uint8_t width, uint32_t* data);
    int sbaWriteN(uint32_t addr, uint8_t width, uint32_t value);
    int sbaReadBlock32(uint32_t addr, uint32_t words, uint32_t* out);
    int absMemRead32(uint32_t addr, uint32_t* val);

    FtdiJtag& j_;
    ErrorState& err_;
    bool ready_ = false;
    int curSbAccess_ = -1;   /* 当前 sbaccess 宽度缓存, 变化时才重写 sbcs */
    bool autoInc_ = false;   /* sbautoincrement+sbreadondata 突发模式(sbaConfigure 探测) */
};

} // namespace gs32

#endif /* GSLINK_RISCV_DEBUG_H */
