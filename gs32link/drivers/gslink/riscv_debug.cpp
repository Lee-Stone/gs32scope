/*
 * 层2实现: RISC-V Debug Spec 0.13.2 — DMI/DM/SBA
 */
#include "riscv_debug.h"
#include "gslink_log.h"
#include <algorithm>
#include <cstring>

namespace gs32 {

/* ---- RISC-V 调试常量 (Debug Spec 0.13) ---- */
namespace {
enum : uint8_t { IR_IDCODE = 0x01, IR_DTMCS = 0x10, IR_DMI = 0x11 };
enum : uint32_t {
    DMI_OP_NOP   = 0,
    DMI_OP_READ  = 1,
    DMI_OP_WRITE = 2,
    DMI_OP_FAIL  = 3,
};
enum : uint32_t {
    DM_DMCONTROL = 0x10, DM_DMSTATUS = 0x11,
    DM_ABSTRACTCS = 0x16, DM_COMMAND = 0x17,
    DM_DATA0     = 0x04,
    DM_SBCS      = 0x38, DM_SBADDRESS0 = 0x39, DM_SBDATA0 = 0x3C,
};
enum : uint32_t {
    DMCONTROL_DMACTIVE = 1u << 0,
};
enum : uint32_t {
    /* sbcs(0x38) 位布局(GS32 权威定义, 实测回读 0x20140407 验证):
     *   [31:29] sbversion  [28:23] 固定0    [22] sbbusyerror  [21] sbbusy
     *   [20] sbreadonaddr  [19:17] sbaccess [16] sbautoincrement
     *   [15] sbreadondata  [14:12] sberror(W1C) [11:5] sbasize
     *   [4] sbaccess128 [3] 64 [2] 32 [1] 16 [0] 8 */
    SBCS_SBACCESS_SHIFT = 17,
    SBCS_SBREADONADDR   = 1u << 20,
    SBCS_SBAUTOINCREMENT= 1u << 16,
    SBCS_SBBUSY         = 1u << 21,
    SBCS_SBERROR_SHIFT  = 12,
    SBCS_SBERROR_W1C    = 0x7u << 12,
    SBCS_SBREADONDATA   = 1u << 15,
};

/* ---- JTAG IDCODE 读取(诊断) ----
 * IDCODE 指令 = 0x01; TAP 复位后 IR 自动装载 IDCODE, 可直接扫 DR 读出。
 * 用于验证: 1) TAP 复位后首读  2) 显式 scanIR(0x01) 锁存后再读(IR Update 路径) */
void logIdcode(const char* tag, bool ok, uint32_t id) {
    if (ok)
        logInfo("[init] JTAG ID (%s) = 0x%08X  [version=%u part=0x%03X mfr=0x%03X]\n",
            tag, id, (id >> 28) & 0xF, (id >> 12) & 0xFFFF, (id >> 1) & 0x7FF);
    else
        logInfo("[init] JTAG ID (%s) read FAILED\n", tag);
}
} // namespace

/* ---- DMI 访问: 41 位 DR = {op[1:0], data[31:0], address[6:0]}, LSB 先出 ---- */
int RiscvDebug::dmiAccess(uint8_t op, uint32_t addr, uint32_t data, uint32_t* outData) {
    const uint64_t dr = (uint64_t)op | ((uint64_t)data << 2) | ((uint64_t)addr << 34);
    uint8_t tdi[6] = {}, tdo[6] = {};
    for (int i = 0; i < 6; ++i) tdi[i] = (uint8_t)(dr >> (8 * i));
    if (!j_.scanIR(IR_DMI)) {
        err_.set(GS32_ERR_LOST, "JTAG IR scan(DMI) failed");
        return GS32_ERR_LOST;
    }
    if (!j_.scanDR(41, tdi, tdo)) {
        err_.set(GS32_ERR_LOST, "JTAG DR scan(DMI) failed");
        return GS32_ERR_LOST;
    }
    /* scanDR 退出序列自带 ≥1 个 IDLE 时钟, 满足 DMI 时序(dtmcs.idle=1), 无需单独 dmiIdle */
    uint64_t r = 0;
    for (int i = 0; i < 6; ++i) r |= (uint64_t)tdo[i] << (8 * i);
    if (outData) *outData = (uint32_t)(r >> 2);
    const int resp = (int)(r & 0x3);
    logDbg("  [dmi] %s addr=0x%X data=0x%08X -> resp=%d%s\n",
        op == DMI_OP_READ ? "R" : op == DMI_OP_WRITE ? "W" : "N",
        addr, data, resp,
        resp == DMI_OP_FAIL ? " (FAILED)" : resp == 2 ? " (BUSY)" : "");
    return resp;
}

int RiscvDebug::dmiWriteReg(uint32_t addr, uint32_t data) {
    int rc = dmiAccess(DMI_OP_WRITE, addr, data, nullptr);
    if (rc == GS32_ERR_LOST) return rc;
    logDbg("[dmi W] addr=0x%02X data=0x%08X%s\n", addr, data,
            rc != DMI_OP_NOP ? " (FAILED/BUSY)" : "");
    return GS32_OK;
}

int RiscvDebug::dmiReadReg(uint32_t addr, uint32_t* data) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        int rc = dmiAccess(DMI_OP_READ, addr, 0, nullptr);
        if (rc == GS32_ERR_LOST) return rc;
        for (int i = 0; i < 32; ++i) {
            uint32_t v = 0;
            rc = dmiAccess(DMI_OP_NOP, addr, 0, &v);
            if (rc == GS32_ERR_LOST) return rc;
            if (rc == DMI_OP_NOP) {
                *data = v;
                logDbg("[dmi R] addr=0x%02X data=0x%08X (nop#%d try#%d)\n",
                        addr, v, i, attempt);
                return GS32_OK;
            }
            if (rc == DMI_OP_FAIL) break;
        }
    }
    char a[16];
    snprintf(a, sizeof(a), "%X", addr);
    err_.set(GS32_ERR_READ, "DMI read FAILED(addr=0x" + std::string(a) + ")");
    return GS32_ERR_READ;
}

/* ---- SBA ----
 * sbcs 只在访问宽度变化时重写(头/尾 8bit 对齐补偿 ↔ 中间 32bit 块):
 * 同宽度连续访问零 sbcs 事务, 写 sbaddress0 自动触发读 */
/* 统一的 sbcs 配置字: 突发模式(autoincrement+readondata)探测支持时始终带上 */
uint32_t RiscvDebug::sbcsCfg_(uint8_t width) const {
    uint32_t cfg = ((uint32_t)width << SBCS_SBACCESS_SHIFT) | SBCS_SBREADONADDR;
    if (autoInc_) cfg |= SBCS_SBAUTOINCREMENT | SBCS_SBREADONDATA;
    return cfg;
}

int RiscvDebug::setSbAccess(uint8_t width) {
    if ((int)width == curSbAccess_) return GS32_OK;
    if (dmiWriteReg(DM_SBCS, sbcsCfg_(width))) { curSbAccess_ = -1; return GS32_ERR_WRITE; }
    curSbAccess_ = width;
    return GS32_OK;
}

int RiscvDebug::sbaConfigure() {
    /* 第一笔: 单独写纯 W1C 清粘滞错误(sberror[14:12] + sbbusyerror[22]).
     * 混在配置位里写清不掉(GS32 实测), 必须独立一笔 */
    if (dmiWriteReg(DM_SBCS, SBCS_SBERROR_W1C | (1u << 22)))
        return GS32_ERR_WRITE;
    /* 第二笔: 基础配置(sbaccess=32bit + readonaddr) */
    if (dmiWriteReg(DM_SBCS, (2u << SBCS_SBACCESS_SHIFT) | SBCS_SBREADONADDR))
        return GS32_ERR_WRITE;
    curSbAccess_ = 2;
    /* 回读确认配置生效 + 解码能力 */
    uint32_t cs = 0;
    if (dmiReadReg(DM_SBCS, &cs)) return GS32_ERR_READ;
    const int sberr = (cs >> SBCS_SBERROR_SHIFT) & 0x7;
    logInfo("[init] SBCS=0x%08X (ver=%d sbaccess=%d sbsize=%d "
            "widths=%s%s%s%s%s readonaddr=%d sberror=%d%s)\n",
            cs, (cs >> 29) & 0x7, (cs >> SBCS_SBACCESS_SHIFT) & 0x7,
            (cs >> 5) & 0x7F,
            (cs & (1u<<4)) ? "128 " : "", (cs & (1u<<3)) ? "64 " : "",
            (cs & (1u<<2)) ? "32 " : "", (cs & (1u<<1)) ? "16 " : "",
            (cs & (1u<<0)) ? "8" : "",
            (cs & SBCS_SBREADONADDR) ? 1 : 0, sberr,
            (cs & (1u<<22)) ? " [busyerror!]" : "");
    if (sberr) {
        err_.set(GS32_ERR_READ, "SBA error sticky after clear");
        return GS32_ERR_READ;
    }
    /* autoincrement 突发模式探测: 写 bit16+bit15 回读判定(spec: 未实现位硬连线 0)。
     * 早期"不支持"结论出自 SBDATA0 错地址(0x3D)时期, 不可信, 此处重测 */
    {
        const uint32_t burst = (2u << SBCS_SBACCESS_SHIFT) | SBCS_SBREADONADDR
                             | SBCS_SBAUTOINCREMENT | SBCS_SBREADONDATA;
        uint32_t rb = 0;
        if (!dmiWriteReg(DM_SBCS, burst) && !dmiReadReg(DM_SBCS, &rb)) {
            autoInc_ = (rb & SBCS_SBAUTOINCREMENT) && (rb & SBCS_SBREADONDATA);
            logInfo("[init] SBA burst autoincrement=%s readondata=%s (rb=0x%08X)\n",
                    (rb & SBCS_SBAUTOINCREMENT) ? "yes" : "no",
                    (rb & SBCS_SBREADONDATA) ? "yes" : "no", rb);
        }
    }
    /* 终态: 统一走 sbcsCfg_(含突发位如支持), 宽度缓存同步 */
    if (dmiWriteReg(DM_SBCS, sbcsCfg_(2))) return GS32_ERR_WRITE;
    curSbAccess_ = 2;
    return GS32_OK;
}

int RiscvDebug::sbWaitIdle() {
    for (int i = 0; i < 64; ++i) {
        uint32_t cs = 0;
        if (dmiReadReg(DM_SBCS, &cs)) return GS32_ERR_READ;
        if (!(cs & SBCS_SBBUSY)) return GS32_OK;
    }
    err_.set(GS32_ERR_TIMEOUT, "SBA busy timeout");
    return GS32_ERR_TIMEOUT;
}

/* SBA 探测: 对比两条读路径(readonaddr vs readondata), 判定 DM 实际行为 */
void RiscvDebug::sbaDiag(uint32_t probe_addr) {
    /* 参照地址: 实际监控地址 */
    const uint32_t addrs[1] = {probe_addr};
    for (int i = 0; i < 1; ++i) {
        uint32_t v = 0;
        int rc = sbaReadN(addrs[i], 2, &v);
        uint32_t cs = 0;
        dmiReadReg(DM_SBCS, &cs);
        logInfo("[sba-diag] readonaddr @0x%08X = 0x%08X (rc=%d sberror=%d)\n",
                addrs[i], v, rc, (cs >> SBCS_SBERROR_SHIFT) & 7);
    }
    /* 路径2: sbreadondata(读 sbdata0 本身触发读), 连读两次覆盖两种语义 */
    const uint32_t cfgData = (2u << SBCS_SBACCESS_SHIFT) | SBCS_SBREADONDATA;
    if (dmiWriteReg(DM_SBCS, cfgData) == GS32_OK) {
        curSbAccess_ = 2;
        if (dmiWriteReg(DM_SBADDRESS0, probe_addr) == GS32_OK) {
            uint32_t v1 = 0, v2 = 0;
            int r1 = dmiReadReg(DM_SBDATA0, &v1);
            sbWaitIdle();
            int r2 = dmiReadReg(DM_SBDATA0, &v2);
            logInfo("[sba-diag] readondata @0x%08X = 0x%08X / 0x%08X (r=%d/%d)\n",
                    probe_addr, v1, v2, r1, r2);
        }
        /* 恢复 readonaddr(+突发位如支持)配置 */
        if (dmiWriteReg(DM_SBCS, sbcsCfg_(2)) == GS32_OK) curSbAccess_ = 2;
    }
}

/* ---- Abstract Command 路径探测 ----
 * SBA 在 GS32 上到不了存储器(sberror=2 全地址), OpenOCD 走的是 abstract command.
 * 探测 Access Memory(cmdtype=2, 0.13.2): 地址放 data0, 发命令, 回读 data0.
 * spec: 该命令可在 hart 运行时执行(示波器场景关键) */
int RiscvDebug::absMemRead32(uint32_t addr, uint32_t* val) {
    /* 清 cmderr(abstracts.cmderr[10:8] W1C) */
    if (dmiWriteReg(DM_ABSTRACTCS, 0x7u << 8)) return GS32_ERR_WRITE;
    /* data0 = 目标地址 */
    if (dmiWriteReg(DM_DATA0, addr)) return GS32_ERR_WRITE;
    /* command = cmdtype=2 | aamsize=2(32bit) | aamwrite=0 */
    if (dmiWriteReg(DM_COMMAND, (2u << 24) | (2u << 20))) return GS32_ERR_WRITE;
    /* 轮询 abstractcs.busy(bit12) — abstract 命令耗时多个总线周期, busy 可观察 */
    uint32_t cs = 0;
    for (int i = 0; i < 64; ++i) {
        if (dmiReadReg(DM_ABSTRACTCS, &cs)) return GS32_ERR_READ;
        if (!(cs & (1u << 12))) break;
    }
    if (cs & (1u << 12)) { err_.set(GS32_ERR_TIMEOUT, "abstract busy"); return GS32_ERR_TIMEOUT; }
    const int cmderr = (cs >> 8) & 7;
    if (cmderr) { err_.set(GS32_ERR_READ, "abstract cmderr=" + std::to_string(cmderr)); return GS32_ERR_READ; }
    if (dmiReadReg(DM_DATA0, val)) return GS32_ERR_READ;
    return GS32_OK;
}

void RiscvDebug::abstractDiag(uint32_t probe_addr) {
    uint32_t cs = 0;
    if (!dmiReadReg(DM_ABSTRACTCS, &cs))
        logInfo("[abs-diag] abstractcs=0x%08X (progbufsize=%d cmderr=%d busy=%d)\n",
                cs, (cs >> 24) & 0xF, (cs >> 8) & 7, (cs >> 12) & 1);
    const uint32_t addrs[1] = {probe_addr};
    for (int i = 0; i < 1; ++i) {
        uint32_t v = 0;
        int rc = absMemRead32(addrs[i], &v);
        logInfo("[abs-diag] AccessMemory @0x%08X = 0x%08X (rc=%d%s)\n",
                addrs[i], v, rc, rc ? " FAILED" : "");
    }
}

int RiscvDebug::sbaReadN(uint32_t addr, uint8_t width, uint32_t* data) {
    if (setSbAccess(width)) return GS32_ERR_READ;
    if (dmiWriteReg(DM_SBADDRESS0, addr)) return GS32_ERR_READ;
    /* 不等 sbbusy: DMI 事务(ms级)远慢于 SBA 总线访问(ns级),
     * 下一笔 JTAG 到达时访问必然已完成, busy 观察不到(实测恒 0) */
    if (dmiReadReg(DM_SBDATA0, data)) return GS32_ERR_READ;
    return GS32_OK;
}

int RiscvDebug::sbaWriteN(uint32_t addr, uint8_t width, uint32_t value) {
    if (setSbAccess(width)) return GS32_ERR_WRITE;
    if (dmiWriteReg(DM_SBADDRESS0, addr)) return GS32_ERR_WRITE;
    if (dmiWriteReg(DM_SBDATA0, value)) return GS32_ERR_WRITE;
    /* 等写完成: 上笔未完成时写下一笔会置 sbbusyerror(实测粘滞的根源) */
    if (sbWaitIdle()) return GS32_ERR_WRITE;
    return GS32_OK;
}

/* 批量快速路径: 连续对齐 32bit 字块读取。
 * 每字 3 笔 DMI 扫描(WRITE sbaddress0 自动触发读 / READ sbdata0 / NOP 取数据),
 * 全部聚合到层1 批量缓冲 → 整块仅 1 笔 USB 写 + 1 笔读, 消除事务间往返。
 * DMI 流水语义: 扫描 N 返回的是操作 N-1 的结果 →
 *   READ 扫描的 tdo = WRITE 的响应; NOP 扫描的 tdo = READ 的响应+数据 */
int RiscvDebug::sbaReadBlock32(uint32_t addr, uint32_t words, uint32_t* out) {
    if (!words) return GS32_OK;
    if (setSbAccess(2)) return GS32_ERR_READ;
    /* 每字 12B tdo: [0..6)=READ 扫描结果(=WRITE 响应), [6..12)=NOP 扫描结果(=READ 响应+数据) */
    std::vector<uint8_t> tdo(12 * (size_t)words);
    auto dr41 = [](uint8_t op, uint32_t dmiAddr, uint32_t data) {
        return (uint64_t)op | ((uint64_t)data << 2) | ((uint64_t)dmiAddr << 34);
    };
    j_.beginBatch();
    for (uint32_t w = 0; w < words; ++w) {
        const uint64_t ops[3] = {
            dr41(DMI_OP_WRITE, DM_SBADDRESS0, addr + 4u * w),   /* 写地址, 触发读 */
            dr41(DMI_OP_READ,  DM_SBDATA0, 0),                   /* 发起读 */
            dr41(DMI_OP_NOP,   0, 0),                            /* 取回数据 */
        };
        for (int k = 0; k < 3; ++k) {
            uint8_t tdi[6] = {};
            for (int i = 0; i < 6; ++i) tdi[i] = (uint8_t)(ops[k] >> (8 * i));
            uint8_t* dst = (k > 0) ? &tdo[12 * (size_t)w + 6 * (size_t)(k - 1)] : nullptr;
            if (!j_.scanDR(41, tdi, dst)) {
                j_.flushBatch();
                err_.set(GS32_ERR_LOST, "JTAG DR scan(DMI batch) failed");
                return GS32_ERR_LOST;
            }
        }
    }
    if (!j_.flushBatch()) {
        err_.set(GS32_ERR_LOST, "USB batch flush failed");
        return GS32_ERR_LOST;
    }
    for (uint32_t w = 0; w < words; ++w) {
        const unsigned respW = tdo[12 * (size_t)w] & 3;          /* WRITE 的响应 */
        uint64_t n = 0;
        for (int i = 0; i < 6; ++i) n |= (uint64_t)tdo[12 * (size_t)w + 6 + i] << (8 * i);
        const unsigned respR = (unsigned)(n & 3);                /* READ 的响应 */
        if (respW || respR) {
            logInfo("[sba-batch] DMI resp error @word %u (W=%u R=%u), fallback slow path\n",
                    w, respW, respR);
            for (uint32_t k = 0; k < words; ++k)
                if (sbaReadN(addr + 4u * k, 2, &out[k])) return GS32_ERR_READ;
            return GS32_OK;
        }
        out[w] = (uint32_t)(n >> 2);
        logDbg("[sba-batch] @0x%08X = 0x%08X\n", addr + 4u * w, out[w]);
    }
    return GS32_OK;
}

/* ---- 初始化 ---- */
int RiscvDebug::ensureReady() {
    if (ready_) return GS32_OK;
    logInfo("[init] TAP reset...\n");
    if (!j_.tapReset()) {
        logInfo("[init] TAP reset FAILED\n");
        err_.set(GS32_ERR_CONNECT, "TAP reset failed");
        return GS32_ERR_CONNECT;
    }
    /* 诊断 1: 单 TAP 时 TAP 复位后 IR 自动装载 IDCODE, 直接读 DR。
     * 多 TAP 时链上是多个 IDCODE, 32bit 直读会错位, 跳过。 */
    uint32_t idcode = 0;
    bool idcodeOk = false;
    if (j_.tapCount() == 1) {
        uint32_t id = 0;
        const bool ok = j_.readDr32(&id);
        logIdcode("after TAP reset", ok, id);
        if (ok) { idcode = id; idcodeOk = true; }
    }
    /* 诊断 2: 显式设置 IR=IDCODE(0x01) 再读, 验证 IR 移位/Update 锁存路径 */
    if (!j_.scanIR(IR_IDCODE)) {
        logInfo("[init] scanIR(IDCODE=0x01) FAILED\n");
    } else {
        uint32_t id = 0;
        const bool ok = j_.readDr32(&id);
        logIdcode("IR=0x01 set", ok, id);
        if (ok) { idcode = id; idcodeOk = true; }
    }
    if (idcodeOk) idcode_ = idcode;

    /* 目标型号校验: JTAG IDCODE 必须与所选型号的配置族一致 */
    if (expectedIdcode_ && !idcodeOk) {
        err_.set(GS32_ERR_CONNECT, "目标 IDCODE 读取失败, 无法校验目标型号");
        return GS32_ERR_CONNECT;
    }
    if (expectedIdcode_ && idcode != expectedIdcode_) {
        char msg[220];
        snprintf(msg, sizeof(msg),
                 "目标不匹配: 选择 %s (IDCODE 0x%08X), 实际 0x%08X",
                 expectedModel_.empty() ? "?" : expectedModel_.c_str(),
                 expectedIdcode_, idcode);
        logInfo("[init] %s\n", msg);
        err_.set(GS32_ERR_CONNECT, msg);
        return GS32_ERR_CONNECT;
    }
    /* DTM 复位: 写 dtmcs.dmihardreset(下面 DTMCS 事务完成) */
    logInfo("[init] DTM reset via DTMCS dmihardreset...\n");
    if (!j_.scanIR(IR_DTMCS)) {
        logInfo("[init] IR scan(DTMCS) FAILED\n");
        err_.set(GS32_ERR_CONNECT, "IR scan(DTMCS) failed");
        return GS32_ERR_CONNECT;
    }
    /* 37xA 的 DM 需要多次 dmihardreset 才稳定(参考厂商 OpenOCD reset_dm) */
    for (int pass = 0; pass < 4; ++pass) {
        /* dtmcs.dmihardreset = bit16 (Debug Spec 0.13); bit17 是 dmireset */
        const uint32_t hardReset = 1u << 16;
        uint8_t tdi[4], tdo[4] = {};
        for (int i = 0; i < 4; ++i) tdi[i] = (hardReset >> (8 * i)) & 0xFF;
        if (!j_.scanDR(32, tdi, tdo)) {
            logInfo("[init] DR scan(DTMCS) FAILED\n");
            err_.set(GS32_ERR_CONNECT, "DR scan(DTMCS) failed");
            return GS32_ERR_CONNECT;
        }
        Sleep(2);
    }
    /* 读回 DTMCS 验证 DTM 通信, 并解析 idle 周期数(DMI 扫描间必须插入) */
    if (j_.scanIR(IR_DTMCS)) {
        uint32_t dc = 0;
        if (j_.readDr32(&dc)) {
            const int idle = (int)((dc >> 12) & 0x7);
            j_.setIdleCycles(idle);
            logInfo("[init] DTMCS=0x%08X (version=%d abits=%d idle=%d)\n",
                    dc, (int)(dc & 0xF), (int)((dc >> 4) & 0x3F), idle);
            /* 多 TAP 链: IR 只移入当前驱动的一位长度, DTMCS 读到的是 IDCODE */
            if (idcodeOk && dc == idcode) {
                logInfo("[init] DTMCS == IDCODE, JTAG 链可能包含多个 TAP\n");
                err_.set(GS32_ERR_CONNECT,
                         "目标 JTAG 链扫描失败: DTMCS 读回 IDCODE（TAP 配置可能不匹配）");
                return GS32_ERR_CONNECT;
            }
        }
    }
    /* 激活前读一次 dmstatus, 判断 DMI 通路是否真的通 */
    {
        uint32_t pre = 0;
        if (dmiReadReg(DM_DMSTATUS, &pre) == GS32_OK)
            logInfo("[init] pre-activate dmstatus=0x%08X\n", pre);
        else
            logInfo("[init] pre-activate dmstatus read FAILED\n");
    }
    logInfo("[init] DM active...\n");
    /* 37xA: 切换 IR 后先发几个 DMI NOP, 给 DTM/DM 一个同步窗口 */
    for (int i = 0; i < 4; ++i)
        dmiAccess(DMI_OP_NOP, DM_DMCONTROL, 0, nullptr);
    if (dmiWriteReg(DM_DMCONTROL, DMCONTROL_DMACTIVE)) {
        logInfo("[init] dmcontrol write FAILED\n");
        return GS32_ERR_CONNECT;
    }
    /* 0.13: dmactive 位在 dmcontrol(bit0), dmstatus 没有此位!
     * 回读 dmcontrol 确认 DM 已激活 */
    for (int i = 0; i < 4; ++i)
        dmiAccess(DMI_OP_NOP, DM_DMCONTROL, 0, nullptr);
    Sleep(2);
    uint32_t ctl = 0;
    bool ok = false;
    for (int i = 0; i < 32; ++i) {
        if (dmiReadReg(DM_DMCONTROL, &ctl)) {
            logInfo("[init] dmcontrol read FAILED (#%d)\n", i);
            return GS32_ERR_CONNECT;
        }
        if (ctl & DMCONTROL_DMACTIVE) { ok = true; break; }
    }
    if (!ok) {
        logInfo("[init] DM not active (dmcontrol.dmactive reads 0)\n");
        err_.set(GS32_ERR_CONNECT, "RISC-V DM not in dmactive");
        return GS32_ERR_CONNECT;
    }
    /* dmstatus 仅作诊断打印(不再作为就绪判据) */
    uint32_t st = 0;
    if (!dmiReadReg(DM_DMSTATUS, &st)) {
        logInfo("[init] dmstatus=0x%08X (ver=%d auth=%d halted=%d havereset=%d)\n",
                st, (int)(st & 0xF), (int)((st >> 7) & 1),
                (int)((st >> 9) & 1), (int)((st >> 19) & 1));
    }
    /* 配置 SBA: sbreadonaddr=1 + 32bit 访问(整个连接生命周期只需一次) */
    /* 在线示波器要求目标运行: 残留调试暂停或系统复位后自动恢复.
     * 只检查 allhalted 不够: 关闭上一次调试会话后目标可能处于
     * havereset(核被复位但 allhalted=0), 变量会全部冻结为 0.
     * ackhavereset(bit28) 清复位标志, resumereq(bit30) 让核继续跑. */
    const bool allHalted = (st & (1u << 9)) != 0;
    const bool hasReset  = (st & (1u << 19)) != 0;   /* allhavereset */
    if (allHalted || hasReset) {
        logInfo("[init] hart %s, resuming...\n",
                allHalted ? "halted" : "in reset");
        uint32_t ctl = DMCONTROL_DMACTIVE | (1u << 30);   /* resumereq */
        if (hasReset) ctl |= (1u << 28);                  /* ackhavereset */
        dmiWriteReg(DM_DMCONTROL, ctl);
        uint32_t s2 = 0;
        for (int i = 0; i < 32; ++i) {
            if (dmiReadReg(DM_DMSTATUS, &s2)) { s2 = 0xDEADDEAD; break; }
            if (!(s2 & (1u << 9)) && !(s2 & (1u << 19))) break;
        }
        logInfo("[init] resume result dmstatus=0x%08X%s\n", s2,
                ((s2 & (1u << 9)) || (s2 & (1u << 19)))
                    ? " (STILL HALTED/RESET - values may be frozen, power-cycle the board)"
                    : " (running)");
    }
    if (sbaConfigure()) {
        logInfo("[init] SBA configure FAILED\n");
        return GS32_ERR_CONNECT;
    }
    ready_ = true;
    err_.clear();
    logInfo("[init] RISC-V DM ready\n");
    return GS32_OK;
}

/* ---- 内存读写 ---- */
int RiscvDebug::readMem(uint32_t addr, uint8_t* dst, uint32_t size) {
    while (size && (addr & 3)) {
        uint32_t v = 0;
        if (sbaReadN(addr, 0, &v)) return GS32_ERR_READ;
        *dst++ = (uint8_t)v;
        ++addr; --size;
    }
    while (size >= 4) {
        const uint32_t words = size / 4;
        /* 中段对齐 32bit 块: 走批量聚合快速路径(x86 主机允许非对齐落点) */
        if (sbaReadBlock32(addr, words, reinterpret_cast<uint32_t*>(dst)))
            return GS32_ERR_READ;
        dst += words * 4; addr += words * 4; size -= words * 4;
    }
    while (size) {
        uint32_t v = 0;
        if (sbaReadN(addr, 0, &v)) return GS32_ERR_READ;
        *dst++ = (uint8_t)v;
        ++addr; --size;
    }
    return GS32_OK;
}

/* ---- 多区域批量读取 ----
 * gs32_read 的多变量(多 region)整体聚合: 全部扫描(宽度切换的 sbcs 写 +
 * 每字节/字的 W addr / R sbdata0 / NOP 三笔)进一个批量缓冲, 单笔 FT_Write/FT_Read。
 * 消除变量间 100µs~1ms 的 ftd2xx 往返间隔(此前每 region 独立 flush)。
 * DMI 流水语义同 sbaReadBlock32; flush 后统一校验响应, 异常整体回退逐区域慢路径 */
int RiscvDebug::readRegions(const MemRegion* regs, uint32_t count, uint8_t* out) {
    if (!count) return GS32_OK;
    /* 阶段0: 预计算扫描总数(一次性预留 tdo 存储, 防 vector 扩容致指针失效)。
     * 突发模式(支持 autoincrement+readondata 时): 每连续段 = 1 笔 W 起始地址 +
     * N 笔 R sbdata0(每读自动触发下一地址读) + 1 笔 NOP 取末字, 计 N+2;
     * 普通模式: 每单元 3 笔(W addr / R data / NOP) */
    size_t nScans = 0;
    int width = curSbAccess_;
    for (uint32_t r = 0; r < count; ++r) {
        const uint32_t addr = regs[r].addr, size = regs[r].size;
        if (!size) continue;
        const uint32_t head = std::min((4 - (addr & 3)) & 3u, size);
        const uint32_t words = (size - head) / 4;
        const uint32_t tail = size - head - words * 4;
        auto seg = [&](uint32_t units, int w) {
            if (!units) return;
            if (w != width) { ++nScans; width = w; }   /* 宽度切换: 1 笔 sbcs 写 */
            nScans += autoInc_ ? (size_t)units + 2 : (size_t)units * 3;
        };
        seg(head, 0); seg(words, 2); seg(tail, 0);
    }
    if (!nScans) return GS32_OK;
    /* 突发模式批末追加 R SBCS + NOP: readondata 使每次 R sbdata0 触发下一地址读,
     * 末次触发的是段末+1 的"越界"读 — 若该地址无效会置粘滞 sberror 并阻断后续
     * SBA 访问(且不反映在 DMI resp 里), 必须每批检查 sberror 以便清除+回退 */
    size_t sbcsCheckScan = SIZE_MAX;                   /* 末位 NOP 的 tdo 槽位 */
    nScans += 2; sbcsCheckScan = nScans - 1;   /* 坏地址在普通模式也置粘滞 sberror, 检查无条件启用 */

    std::vector<uint8_t> tdoFlat(6 * nScans);          /* 每扫描 6B tdo 落点 */
    struct Rec { uint8_t* tdo; uint8_t* dst; uint8_t nBytes; };
    std::vector<Rec> recs;
    recs.reserve(nScans);
    size_t si = 0;
    auto dr41 = [](uint64_t op, uint32_t a, uint32_t d) {
        return op | ((uint64_t)d << 2) | ((uint64_t)a << 34);
    };
    auto emit = [&](uint64_t frame, uint8_t* dst, uint8_t nBytes) -> bool {
        uint8_t tdi[6] = {};
        for (int i = 0; i < 6; ++i) tdi[i] = (uint8_t)(frame >> (8 * i));
        recs.push_back({&tdoFlat[6 * si], dst, nBytes});
        return j_.scanDR(41, tdi, &tdoFlat[6 * si++]);
    };
    /* 普通模式: 每单元 3 笔 */
    auto emitUnit = [&](uint32_t addr, int w, uint8_t* dst, uint8_t nBytes) -> bool {
        if (!emit(dr41(DMI_OP_WRITE, DM_SBADDRESS0, addr), nullptr, 0)) return false;
        if (!emit(dr41(DMI_OP_READ,  DM_SBDATA0, 0), nullptr, 0)) return false;
        return emit(dr41(DMI_OP_NOP, 0, 0), dst, nBytes);
    };
    /* 突发模式: 1 笔 W 起始地址(readonaddr 触发首读) + N 笔 R sbdata0
     * (readondata: 每次读触发下一地址读, autoincrement: 地址按 sbaccess 递增)
     * + 1 笔 NOP 取末单元。
     * DMI 流水: 扫描 k 输出 = 操作 k-1 结果 → 单元 u 的数据出现在扫描 u+2,
     * 即 R 扫描序号 k(1 起) 承载单元 k-2, 末单元由 NOP 承载 */
    auto emitSeg = [&](uint32_t start, uint32_t units, int w, uint8_t* dst,
                       uint32_t ub) -> bool {
        if (!units) return true;                       /* 空段: 不切宽度不发包 */
        if (w != width) {
            if (!emit(dr41(DMI_OP_WRITE, DM_SBCS, sbcsCfg_((uint8_t)w)), nullptr, 0))
                return false;
            width = w;
            curSbAccess_ = w;                          /* sbcs 写已上线, 缓存同步 */
        }
        if (!autoInc_) {
            for (uint32_t u = 0; u < units; ++u)
                if (!emitUnit(start + u * ub, w, dst + u * ub, (uint8_t)ub)) return false;
            return true;
        }
        if (!emit(dr41(DMI_OP_WRITE, DM_SBADDRESS0, start), nullptr, 0)) return false;
        for (uint32_t k = 1; k <= units; ++k) {
            uint8_t* d = (k >= 2) ? dst + (k - 2) * ub : nullptr;
            if (!emit(dr41(DMI_OP_READ, DM_SBDATA0, 0), d, (uint8_t)ub)) return false;
        }
        return emit(dr41(DMI_OP_NOP, 0, 0), dst + (units - 1) * ub, (uint8_t)ub);
    };

    /* 阶段1: 全部扫描进批量缓冲 */
    j_.beginBatch();
    uint8_t* p = out;
    bool ok = true;
    for (uint32_t r = 0; r < count && ok; ++r) {
        const uint32_t addr = regs[r].addr;
        const uint32_t size = regs[r].size;
        if (!size) continue;
        const uint32_t head = std::min((4 - (addr & 3)) & 3u, size);
        const uint32_t words = (size - head) / 4;
        const uint32_t tail = size - head - words * 4;
        ok = emitSeg(addr, head, 0, p, 1);
        if (ok) ok = emitSeg(addr + head, words, 2, p + head, 4);
        if (ok) ok = emitSeg(addr + head + words * 4, tail, 0, p + head + words * 4, 1);
        p += size;
    }
    /* 批末 sberror 检查扫描(突发模式): R SBCS + NOP(其 tdo 即 sbcs 值) */
    if (ok) {
        if (!emit(dr41(DMI_OP_READ, DM_SBCS, 0), nullptr, 0)) ok = false;
        if (ok && !emit(dr41(DMI_OP_NOP, 0, 0), nullptr, 0)) ok = false;
    }
    if (ok) ok = j_.flushBatch();
    if (!ok) {
        curSbAccess_ = -1;                              /* 链路可疑, 宽度缓存失效 */
        err_.set(GS32_ERR_LOST, "JTAG multi-region batch failed");
        return GS32_ERR_LOST;
    }

    /* 阶段2: 统一校验响应码 + 提取数据 */
    for (const auto& rec : recs) {
        const unsigned resp = rec.tdo[0] & 3;
        if (resp) {
            logDbg("[sba-multi] DMI resp=%u, disable burst + fallback slow path\n", resp);
            autoInc_ = false;
            sbWaitIdle();   /* 先等飞行中的坏地址访问落地, 否则清错后错误又置上 */
            curSbAccess_ = -1;
            dmiWriteReg(DM_SBCS, SBCS_SBERROR_W1C | (1u << 22));
            uint8_t* q = out;
            for (uint32_t r = 0; r < count; ++r) {
                if (!regs[r].size) continue;
                if (readMem(regs[r].addr, q, regs[r].size)) return GS32_ERR_READ;
                {   /* 坏地址 region 读后置粘滞 sberror, 不清会阻断后续 region/下一批 */
                    uint32_t cs = 0;
                    if (!dmiReadReg(DM_SBCS, &cs) &&
                        (((cs >> SBCS_SBERROR_SHIFT) & 7) || (cs & (1u << 22))))
                    /* 先等飞行中的坏地址访问落地(sbbusy), 再清粘滞错; 顺序反了错误会清后又置 */
                    sbWaitIdle();
                    dmiWriteReg(DM_SBCS, SBCS_SBERROR_W1C | (1u << 22));
                    /* 纯 W1C 清错写会顺带清掉配置位(readonaddr 等), 必须重写恢复,
                     * 否则宽度缓存误判"无需重写", 后续 W addr 不再触发读 */
                    dmiWriteReg(DM_SBCS, sbcsCfg_(2));
                    curSbAccess_ = 2;
                }
                q += regs[r].size;
            }
            return GS32_OK;
        }
        if (!rec.dst) continue;
        uint64_t n = 0;
        for (int i = 0; i < 6; ++i) n |= (uint64_t)rec.tdo[i] << (8 * i);
        const uint32_t v = (uint32_t)(n >> 2);
        if (rec.nBytes == 4) memcpy(rec.dst, &v, 4);
        else rec.dst[0] = (uint8_t)v;
        logDbg("[sba-multi] @%p = 0x%08X\n", (void*)rec.dst, v);
    }
    /* 阶段3(突发模式): 检查批末 sbcs — 越界触发读若置了 sberror(粘滞, 不反映在
     * DMI resp), 清除并整体回退慢路径重读(错误状态下本批数据不可信) */
    if (sbcsCheckScan != SIZE_MAX) {
        const uint8_t* t = &tdoFlat[6 * sbcsCheckScan];
        uint64_t n = 0;
        for (int i = 0; i < 6; ++i) n |= (uint64_t)t[i] << (8 * i);
        const uint32_t cs = (uint32_t)(n >> 2);
        const int sberr = (cs >> SBCS_SBERROR_SHIFT) & 7;
        if (sberr || (cs & (1u << 22))) {
            logDbg("[sba-multi] sberror=%d sbbusyerror=%d after burst, "
                    "clear + fallback slow path\n", sberr, (int)((cs >> 22) & 1));
            /* 检出即关突发: 慢路径仍带 autoincrement+readondata 会反复投机越界读 */
            autoInc_ = false;
            sbWaitIdle();   /* 先等飞行中的坏地址访问落地, 否则清错后错误又置上 */
            curSbAccess_ = -1;
            dmiWriteReg(DM_SBCS, SBCS_SBERROR_W1C | (1u << 22));
            uint8_t* q = out;
            for (uint32_t r = 0; r < count; ++r) {
                if (!regs[r].size) continue;
                if (readMem(regs[r].addr, q, regs[r].size)) return GS32_ERR_READ;
                {   /* 坏地址 region 读后置粘滞 sberror, 不清会阻断后续 region/下一批 */
                    uint32_t cs = 0;
                    if (!dmiReadReg(DM_SBCS, &cs) &&
                        (((cs >> SBCS_SBERROR_SHIFT) & 7) || (cs & (1u << 22))))
                    /* 先等飞行中的坏地址访问落地(sbbusy), 再清粘滞错; 顺序反了错误会清后又置 */
                    sbWaitIdle();
                    dmiWriteReg(DM_SBCS, SBCS_SBERROR_W1C | (1u << 22));
                    /* 纯 W1C 清错写会顺带清掉配置位(readonaddr 等), 必须重写恢复,
                     * 否则宽度缓存误判"无需重写", 后续 W addr 不再触发读 */
                    dmiWriteReg(DM_SBCS, sbcsCfg_(2));
                    curSbAccess_ = 2;
                }
                q += regs[r].size;
            }
        }
    }
    return GS32_OK;
}

int RiscvDebug::writeMem(uint32_t addr, const uint8_t* src, uint32_t size) {
    while (size && (addr & 3)) {
        if (sbaWriteN(addr, 0, *src++)) return GS32_ERR_WRITE;
        ++addr; --size;
    }
    while (size >= 4) {
        uint32_t v;
        memcpy(&v, src, 4);
        if (sbaWriteN(addr, 2, v)) return GS32_ERR_WRITE;
        src += 4; addr += 4; size -= 4;
    }
    while (size) {
        if (sbaWriteN(addr, 0, *src++)) return GS32_ERR_WRITE;
        ++addr; --size;
    }
    return GS32_OK;
}

} // namespace gs32
