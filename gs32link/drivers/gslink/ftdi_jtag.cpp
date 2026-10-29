/*
 * 层1实现: FTDI D2XX + MPSSE 位流 -> JTAG TAP 状态机
 * 自实现最小 JTAG 位流, 不依赖 libMPSSE。
 * 参考 OpenOCD src/jtag/drivers/mpsse.c 的 mpsse_clock_tms_cs / mpsse_clock_data
 */
#include "ftdi_jtag.h"
#include "gslink_log.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace gs32 {

enum : unsigned long { FT_OK = 0, FT_PURGE_RX = 1, FT_PURGE_TX = 2 };

/* ---- 位辅助(LSB first) ---- */
static bool bitAt8(const uint8_t* b, int idx) {
    return (b[idx >> 3] >> (idx & 7)) & 1;
}

/* ---- D2xxApi: DLL 查找与加载 ---- */
static std::string findFtd2xxDll() {
    {
        HMODULE self = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&findFtd2xxDll), &self)) {
            char dir[MAX_PATH] = {};
            if (GetModuleFileNameA(self, dir, MAX_PATH)) {
                std::string p(dir);
                const size_t s = p.find_last_of("\\/");
                if (s != std::string::npos) {
                    p = p.substr(0, s + 1) + "Ftd2xx.dll";
                    if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                        return p;
                }
            }
        }
    }
    {
        char buf[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableA("GS32LINK_GSLINK_PATH", buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::string p(buf);
            if (p.find("Ftd2xx.dll") == std::string::npos)
                p += (p.back() == '\\' || p.back() == '/') ? "Ftd2xx.dll"
                                                           : "\\Ftd2xx.dll";
            if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                return p;
        }
    }
    return "Ftd2xx.dll";
}

bool D2xxApi::load(std::string& err) {
    if (mod) return true;
    const std::string dllPath = findFtd2xxDll();
    mod = LoadLibraryA(dllPath.c_str());
    if (!mod) {
        err = "LoadLibrary(" + dllPath + ") failed; install FTDI D2XX driver or copy Ftd2xx.dll";
        return false;
    }
    struct Entry { const char* name; void** pfn; };
    const Entry table[] = {
        {"FT_Open",        (void**)&Open},
        {"FT_OpenEx",      (void**)&OpenEx},
        {"FT_ListDevices", (void**)&ListDevices},
        {"FT_Close",       (void**)&Close},
        {"FT_Write",       (void**)&Write},
        {"FT_Read",        (void**)&Read},
        {"FT_SetBitMode",  (void**)&SetBitMode},
        {"FT_SetTimeouts", (void**)&SetTimeouts},
        {"FT_ResetDevice", (void**)&ResetDevice},
        {"FT_Purge",       (void**)&Purge},
    };
    std::string missing;
    for (const auto& e : table)
        if (!(*e.pfn = (void*)GetProcAddress(mod, e.name)))
            missing += std::string(e.name) + " ";
    if (!missing.empty()) {
        err = "Ftd2xx.dll missing symbols: " + missing;
        FreeLibrary(mod); mod = nullptr;
        return false;
    }
    return true;
}

bool FtdiJtag::listPorts(std::vector<std::string>& out) {
    std::string err;
    if (!api_.load(err)) return false;
    unsigned long n = 0;
    /* FT_LIST_NUMBER_ONLY: 仅查询设备数 */
    if (!api_.ListDevices ||
        api_.ListDevices(&n, nullptr, 0x80000000u) != 0 || n == 0)
        return true;   /* 无设备 */
    if (n > 64) n = 64;
    /* FT_LIST_ALL|FT_OPEN_BY_DESCRIPTION: pArg1=char*[] (每项 64B), pArg2=&数量 */
    std::vector<char> buf(n * 64, 0);
    std::vector<char*> ptrs(n, nullptr);
    for (unsigned long i = 0; i < n; ++i) ptrs[i] = &buf[i * 64];
    unsigned long cnt = n;
    if (api_.ListDevices(ptrs.data(), &cnt, 0x20000002u) != 0) return false;
    for (unsigned long i = 0; i < cnt && i < n; ++i)
        out.push_back(std::string(ptrs[i]));
    return true;
}

/* ---- 打开/关闭 ---- */
bool FtdiJtag::open(uint32_t speed_khz, const char* port, std::string& err) {
    if (!api_.load(err)) return false;
    bool ok = false;
    if (port && *port) {
        /* 按描述打开指定端口(FT_OPEN_BY_DESCRIPTION=2); 失败回退设备0 */
        if (api_.OpenEx && api_.OpenEx((void*)port, 2, &ft_) == 0) {
            ok = true;
        } else {
            logInfo("[open] FT_OpenEx(%s) failed, fallback to device 0\n", port);
        }
    }
    if (!ok && api_.Open(0, &ft_) != 0) {
        err = "FT_Open failed: check FT2232H connected & FTDI driver installed";
        logInfo("[open] FT_Open FAILED\n");
        return false;
    }
    logInfo("[open] FT_Open OK (port=%s)\n", (port && *port) ? port : "0");
    if (api_.ResetDevice(ft_) != FT_OK ||
        api_.Purge(ft_, FT_PURGE_RX | FT_PURGE_TX) != FT_OK ||
        api_.SetTimeouts(ft_, 2000, 2000) != FT_OK ||
        api_.SetBitMode(ft_, 0x0B, 0x02) != FT_OK) {
        err = "FTDI MPSSE init failed";
        close();
        return false;
    }
    Sleep(50);
    /* FT2232H: disable divide-by-5(0x8A) 后 60MHz, TCK = 60MHz/2/(1+div) = 30MHz/(1+div) */
    const uint32_t div = speed_khz >= 30000
                       ? 0 : (30000 + speed_khz - 1) / speed_khz - 1;
    const uint8_t mpsseCfg[] = {
        0x85, 0x8A,
        0x80, 0x0A, 0x0B,   /* data=0x0A: TMS(AD3)=1, TDI(AD1)=1(空闲高), dir=0x0B */
        0x86, (uint8_t)(div & 0xFF), (uint8_t)(div >> 8),
        0x87,
    };
    unsigned long w = 0;
    api_.Write(ft_, (void*)mpsseCfg, sizeof(mpsseCfg), &w);
    api_.Purge(ft_, FT_PURGE_RX);
    logInfo("[open] MPSSE init: div=%u written=%lu cfg=[%s]\n", div, w,
            hexDump(mpsseCfg, sizeof(mpsseCfg)).c_str());
    return true;
}

void FtdiJtag::close() {
    if (ft_) api_.Close(ft_);
    ft_ = nullptr;
    if (api_.mod) FreeLibrary(api_.mod);
    api_.mod = nullptr;
    curIr_ = -1;
}

/* ---- USB 收发 ---- */
bool FtdiJtag::writeFull_(const void* buf, uint32_t n) {
    uint32_t sent = 0;
    while (sent < n) {
        unsigned long w = 0;
        if (api_.Write(ft_, (void*)((const char*)buf + sent), n - sent, &w) != FT_OK)
            return false;
        if (!w) return false;
        sent += (uint32_t)w;
    }
    return true;
}

bool FtdiJtag::readFull_(void* buf, uint32_t n) {
    uint32_t got = 0;
    while (got < n) {
        unsigned long r = 0;
        if (api_.Read(ft_, (char*)buf + got, n - got, &r) != FT_OK) return false;
        if (!r) return false;
        got += (uint32_t)r;
    }
    return true;
}

/* ---- MPSSE 位流原语 ----
 * 0x4B = Clock Data Bits Out on -ve edge LSB(TMS/CS 模式):
 *        每数据字节 bit6..0 = 最多 7 个 TMS bit(LSB first), bit7 = TDI(静态), 无读回
 * 0x6B = 同上带读回(每字节 1 bit, TDO 在 bit7)
 * 0x1B = Clock Data Bits Out on -ve edge LSB(普通数据): 每字节 bit0=1bit TDI, 1 TCK, 无读
 * 0x3B = 同上带读
 * 0x19/0x39 = 字节级数据移位(LSB first), 无读/带读(每字节返回 1B)
 */
void FtdiJtag::appendTms_(std::vector<uint8_t>& cmd, uint8_t tmsBits, size_t n,
                          bool tdiHigh) {
    size_t off = 0;
    while (off < n) {
        const size_t chunk = std::min(n - off, (size_t)7);
        cmd.push_back(0x4B);
        cmd.push_back((uint8_t)(chunk - 1));
        uint8_t data = tdiHigh ? 0x80u : 0x00u;
        for (size_t i = 0; i < chunk; ++i)
            if ((tmsBits >> (off + i)) & 1) data |= (uint8_t)(1u << i);
        cmd.push_back(data);
        off += chunk;
    }
}

/* 原语1: TMS 状态迁移。0x4B 每块最多 7 bit TMS(bit0..6), TDI 静态(bit7), 无读 */
bool FtdiJtag::tmsShift_(const std::vector<bool>& tms, bool /*tdi*/) {
    const size_t n = tms.size();
    if (n == 0) return true;
    std::vector<uint8_t> cmd;
    cmd.reserve(n / 7 * 3 + 4);
    size_t off = 0;
    while (off < n) {
        const size_t chunk = std::min(n - off, (size_t)7);
        cmd.push_back(0x4B);
        cmd.push_back((uint8_t)(chunk - 1));
        uint8_t data = 0x80u;   /* TDI 固定高: TMS 状态迁移时 TDI 应保持高(IR/DR 最后位由 tmsShiftLast_ 处理) */
        for (size_t i = 0; i < chunk; ++i)
            if (tms[off + i]) data |= (uint8_t)(1u << i);
        cmd.push_back(data);
        off += chunk;
    }
    cmd.push_back(0x87);
    logDbg("[tx TMS %zu bits, %zu B] %s\n", n, cmd.size(),
        hexDump(cmd.data(), cmd.size()).c_str());
    api_.Purge(ft_, FT_PURGE_RX);
    return writeFull_(cmd.data(), (uint32_t)cmd.size());
}

/* 移位最后 1 位并退出 Shift: 该时钟同时移最后一位数据(bit7)、
 * TMS=1(bit0)使 TAP 进入 Exit1-IR/DR(避免多移 1 位导致锁存值右移)。
 * 需读回最后一位 TDO 时用 0x6B(TMS with read), 否则用 0x4B(无读) */
bool FtdiJtag::tmsShiftLast_(bool tdi, uint8_t* tdoBit) {
    const bool read = tdoBit != nullptr;
    const uint8_t cmd[] = {
        (uint8_t)(read ? 0x6B : 0x4B), 0x00,
        (uint8_t)((tdi ? 0x80u : 0x00u) | 0x01u),
        0x87,
    };
    logDbg("[tx LAST] %02X 00 %02X 87\n", cmd[0], cmd[2]);
    api_.Purge(ft_, FT_PURGE_RX);
    if (!writeFull_(cmd, sizeof(cmd))) return false;
    if (read) {
        uint8_t rx = 0;
        if (!readFull_(&rx, 1)) return false;
        *tdoBit = (rx >> 7) & 1;
    }
    return true;
}

/* 原语2: 数据移位(Shift-DR/IR)。逐 bit 0x3B(带读)/0x1B(无读),
 * TDI=bit0, 每字节 1 个 TCK; 带读时 TDO 读回在每字节 bit7 */
bool FtdiJtag::dataShift_(const uint8_t* tdiBytes, uint32_t bits, uint8_t* tdoOut) {
    if (bits == 0) return true;
    const bool read = tdoOut != nullptr;
    std::vector<uint8_t> cmd;
    cmd.reserve(bits * 3 + 4);
    for (uint32_t i = 0; i < bits; ++i) {
        cmd.push_back(read ? 0x3B : 0x1B);
        cmd.push_back(0x00);                          /* length = 1 bit - 1 */
        cmd.push_back(bitAt8(tdiBytes, (int)i) ? 0x01u : 0x00u);
    }
    cmd.push_back(0x87);
    logDbg("[tx DATA %u bits, %zu B] %s\n", bits, cmd.size(),
        hexDump(cmd.data(), cmd.size()).c_str());
    api_.Purge(ft_, FT_PURGE_RX);
    if (!writeFull_(cmd.data(), (uint32_t)cmd.size())) return false;
    if (read) {
        std::vector<uint8_t> rx(bits);
        if (!readFull_(rx.data(), bits)) return false;
        logDbg("[rx DATA %zu B] %s\n", rx.size(), hexDump(rx.data(), rx.size()).c_str());
        for (uint32_t i = 0; i < bits; ++i)
            if ((rx[i] >> 7) & 1)
                tdoOut[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
    return true;
}

/* ---- TAP 状态机 ---- */
bool FtdiJtag::tapReset() {
    if (batching_ && !flushBatch()) return false;
    std::vector<bool> tms(25, true);
    tms[24] = false;                          /* Test-Logic-Reset -> IDLE */
    const bool ok = tmsShift_(tms, false);
    curIr_ = -1;                              /* TLR 装载 IDCODE, IR 缓存失效 */
    return ok;
}

/* IR 扫描: 单笔通讯 IDLE→Select-IR→Capture-IR→Shift-IR(移位)→Exit1→Update-IR→IDLE */
bool FtdiJtag::scanIRn(int len, uint8_t ir) {
    if (len <= 0) return true;
    if (batching_ && !flushBatch()) return false;   /* IR 扫描不参与聚合 */
    uint8_t tdi[4] = {0, 0, 0, 0};
    for (int i = 0; i < len; ++i)
        if ((ir >> i) & 1) tdi[i >> 3] |= (uint8_t)(1u << (i & 7));
    std::vector<uint8_t> cmd;
    cmd.reserve(24);
    appendTms_(cmd, 0x03, 4, true);              /* IDLE→Select-IR→Capture-IR→Shift-IR */
    /* 前 len-1 位: 整字节用 0x19(3B头+数据, LSB first), 余位逐位 0x1B */
    uint32_t idx = 0;
    const uint32_t whole = (uint32_t)(len - 1) / 8;
    if (whole) {
        cmd.push_back(0x19);
        cmd.push_back((uint8_t)((whole - 1) & 0xFF));
        cmd.push_back((uint8_t)(((whole - 1) >> 8) & 0xFF));
        for (uint32_t b = 0; b < whole; ++b) cmd.push_back(tdi[b]);
        idx = whole * 8;
    }
    for (; idx < (uint32_t)(len - 1); ++idx) {
        cmd.push_back(0x1B);
        cmd.push_back(0x00);
        cmd.push_back(bitAt8(tdi, (int)idx) ? 0x01u : 0x00u);
    }
    /* 最后 1 位: TMS=1, 移位同时 Shift-IR→Exit1-IR */
    cmd.push_back(0x4B);
    cmd.push_back(0x00);
    cmd.push_back((bitAt8(tdi, len - 1) ? 0x80u : 0x00u) | 0x01u);
    appendTms_(cmd, 0x01, 5, true);              /* Exit1→Update-IR(锁存)→IDLE+idle */
    cmd.push_back(0x87);
    logDbg("[tx IR 0x%02X %d bits, %zu B] %s\n", ir, len, cmd.size(),
           hexDump(cmd.data(), cmd.size()).c_str());
    return writeFull_(cmd.data(), (uint32_t)cmd.size());
}

bool FtdiJtag::scanIR(uint8_t ir) {
    if (curIr_ == (int)ir) return true;          /* IR 未变: 跳过重扫 */
    if (!scanIRn(irLen_, ir)) { curIr_ = -1; return false; }
    curIr_ = ir;
    return true;
}

/* TDO 解码: 0x39 段每字节 LSB first 直接同构, 逐位/末位段 TDO 在各字节 bit7 */
static void decodeDr_(const uint8_t* rx, uint32_t bits, uint8_t* tdo) {
    const uint32_t whole = (bits - 1) / 8;
    memset(tdo, 0, (bits + 7) / 8);
    for (uint32_t b = 0; b < whole; ++b) tdo[b] = rx[b];
    for (uint32_t i = whole * 8; i < bits; ++i)
        if ((rx[whole + (i - whole * 8)] >> 7) & 1)
            tdo[i >> 3] |= (uint8_t)(1u << (i & 7));
}

/* DR 扫描: 单笔通讯 IDLE→Select-DR→Capture-DR→Shift-DR(移位+读)→Exit1→Update-DR→IDLE
 * 数据段用 0x39/0x19 字节命令(一次移 whole 字节, 大幅压缩命令流),
 * 余位逐位 0x3B/0x1B, 末位 0x6B/0x4B(TMS=1 退 Shift)。
 * 读回: 0x39 每字节返回 1B(bit0=首TDO, LSB first, 与 tdo 字节直接对齐),
 *       逐位/末位每时钟返回 1B(TDO 在 bit7), 共 bits 字节一次取回。
 * 批量模式: 命令流挂入 batchCmd_, TDO 在 flushBatch() 统一取回解码 */
bool FtdiJtag::scanDR(uint32_t bits, const uint8_t* tdi, uint8_t* tdo) {
    if (bits == 0) {
        if (batching_ && !flushBatch()) return false;
        return tmsShift_({true, false, true, false, false}, false);
    }
    const bool wantRead = tdo != nullptr;
    std::vector<uint8_t> cmd;
    cmd.reserve(4 + (size_t)bits / 8 + (size_t)(bits % 8) * 3 + 16);
    appendTms_(cmd, 0x01, 3, true);              /* IDLE→Select-DR→Capture-DR→Shift-DR */
    const uint32_t whole = (bits - 1) / 8;      /* 前 bits-1 位中的整字节数 */
    uint32_t idx = 0;
    if (whole) {
        cmd.push_back(wantRead ? 0x39 : 0x19);
        cmd.push_back((uint8_t)((whole - 1) & 0xFF));
        cmd.push_back((uint8_t)(((whole - 1) >> 8) & 0xFF));
        for (uint32_t b = 0; b < whole; ++b) cmd.push_back(tdi[b]);
        idx = whole * 8;
    }
    for (; idx < bits - 1; ++idx) {             /* 余位(≤7): 逐位移位 */
        cmd.push_back(wantRead ? 0x3B : 0x1B);
        cmd.push_back(0x00);
        cmd.push_back(bitAt8(tdi, (int)idx) ? 0x01u : 0x00u);
    }
    /* 最后 1 位: TMS=1, 移位同时 Shift-DR→Exit1-DR(带读用 0x6B) */
    cmd.push_back(wantRead ? 0x6B : 0x4B);
    cmd.push_back(0x00);
    cmd.push_back((bitAt8(tdi, (int)(bits - 1)) ? 0x80u : 0x00u) | 0x01u);
    /* Exit1→Update-DR(锁存)→IDLE + N 个 IDLE 周期(DMI 时序要求)
     * dtmcs.idle=7 → 至少 6 个 IDLE cycle(需要数 = idle - 1),
     * 此处发 idleCycles_ 个, 留 1 个余量 */
    const size_t zeros = (size_t)idleCycles_ + 1;  /* +1: 进 IDLE 的那拍 */
    appendTms_(cmd, 0x01, 1 + zeros, true);
    cmd.push_back(0x87);

    if (batching_) {
        /* 聚合: 挂入批量缓冲(超限自动分段 flush), tdo 延迟到 flush 时填充 */
        if (batchCmd_.size() + cmd.size() > kBatchMax) {
            if (!flushBatch()) return false;
            beginBatch();                      /* 分段后重新进入聚合模式 */
        }
        const uint32_t rxB = wantRead ? whole + ((bits - 1) - whole * 8) + 1 : 0;
        batchCmd_.insert(batchCmd_.end(), cmd.begin(), cmd.end());
        batchScans_.push_back({bits, tdo, rxB});
        return true;
    }

    logDbg("[tx DR %u bits, %zu B] %s\n", bits, cmd.size(),
           hexDump(cmd.data(), cmd.size()).c_str());
    if (!writeFull_(cmd.data(), (uint32_t)cmd.size())) return false;
    if (wantRead) {
        /* 返回字节数 = 0x39段 whole 字节(每字节8采样) + 逐位段 tail 字节 + 末位 1 字节 */
        const uint32_t tail = (bits - 1) - whole * 8;
        const uint32_t rxB = whole + tail + 1;
        uint8_t rx[64] = {};
        if (rxB > sizeof(rx)) return false;
        if (!readFull_(rx, rxB)) return false;
        logDbg("[rx DR %u B] %s\n", rxB, hexDump(rx, rxB).c_str());
        decodeDr_(rx, bits, tdo);
    }
    return true;
}

/* ---- 批量聚合 ---- */
void FtdiJtag::beginBatch() {
    if (batching_) return;
    batching_ = true;
    batchCmd_.clear();
    batchScans_.clear();
}

bool FtdiJtag::flushBatch() {
    if (!batching_) return true;
    if (batchCmd_.empty()) { batching_ = false; return true; }
    uint32_t totalRx = 0;
    for (const auto& s : batchScans_) totalRx += s.rxB;
    logDbg("[tx BATCH %zu B, %zu scans, %u B rx] %s\n", batchCmd_.size(),
           batchScans_.size(), totalRx, hexDump(batchCmd_.data(), batchCmd_.size()).c_str());
    std::vector<uint8_t> rx(totalRx);
    /* 插桩: FT_Write 耗时(命令流到达设备的提交延迟, 采样抖动排查用) */
    const auto w0 = std::chrono::steady_clock::now();
    const bool wOk = writeFull_(batchCmd_.data(), (uint32_t)batchCmd_.size());
    lastWriteMs_ = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - w0).count();
    const bool ok = wOk && (!totalRx || readFull_(rx.data(), totalRx));
    if (!ok) {
        batchCmd_.clear();
        batchScans_.clear();
        batching_ = false;
        curIr_ = -1;                       /* 链路可疑, IR 缓存失效 */
        return false;
    }
    if (totalRx) logDbg("[rx BATCH %u B] %s\n", totalRx, hexDump(rx.data(), totalRx).c_str());
    uint32_t off = 0;
    for (auto& s : batchScans_) {
        if (s.tdo && s.rxB) decodeDr_(rx.data() + off, s.bits, s.tdo);
        off += s.rxB;
    }
    batchCmd_.clear();
    batchScans_.clear();
    batching_ = false;
    return true;
}

bool FtdiJtag::readDr32(uint32_t* v) {
    uint8_t tdi[4] = {}, tdo[4] = {};
    if (!scanDR(32, tdi, tdo)) return false;
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i) r |= (uint32_t)tdo[i] << (8 * i);
    if (v) *v = r;
    return true;
}

/* ---- 诊断 ---- */
/* 回读当前 IR 值: 进 Shift-IR, 移位时 TDO 输出 Capture-IR 捕获的 IR(LSB first) */
void FtdiJtag::readBackIR() {
    if (!tmsShift_({true, true, false, false}, false)) return;  /* 进 Shift-IR */
    uint8_t tdi[2] = {0, 0}, tdo[2] = {0, 0};
    if (!dataShift_(tdi, (uint32_t)irLen_, tdo)) return;
    /* 注意: 必须用 TMS=1 退出 Shift-IR({0,0} 会卡在 Shift-IR 污染后续状态) */
    if (!tmsShiftLast_(false, nullptr)) return;   /* Shift-IR -> Exit1-IR */
    if (!tmsShift_({true, false}, false)) return;  /* Exit1-IR --1--> Update-IR --0--> IDLE */
    uint32_t ir = 0;
    for (int i = 0; i < irLen_; ++i)
        if ((tdo[i >> 3] >> (i & 7)) & 1) ir |= (1u << i);
    logInfo("[init] readback IR = 0x%X (len=%d)\n", ir, irLen_);
}

/* IR 长度探测: 进 Shift-IR 移入特征串, TDO 回读中特征串出现的偏移 = IR 长度 */
int FtdiJtag::probeIrLength() {
    if (!tmsShift_({true, true, false, false}, false)) return -1;  /* IDLE->Shift-IR */
    uint8_t pat[2] = {0x55, 0xAA};   /* 位: 1,0,1,0,1,0,1,0, 0,1,0,1,0,1,0,1 */
    uint8_t tdo[2] = {0, 0};
    if (!dataShift_(pat, 15, tdo)) return -1;
    uint8_t last = 0;
    if (!tmsShiftLast_(bitAt8(pat, 15), &last)) return -1;
    tdo[1] |= last ? (uint8_t)(1u << 7) : 0;   /* 第 16 位 TDO 放 tdo[1] bit7 */
    if (!tmsShift_({true, false}, false)) return -1;  /* Exit1-IR --1--> Update-IR --0--> IDLE */
    logInfo("[init] IR probe tx=55AA rx=%02X %02X\n", tdo[0], tdo[1]);
    bool tb[16];
    const bool pb[16] = {1,0,1,0,1,0,1,0, 0,1,0,1,0,1,0,1};
    for (int i = 0; i < 16; ++i) tb[i] = (tdo[i >> 3] >> (i & 7)) & 1;
    for (int off = 0; off <= 8; ++off) {
        bool ok = true;
        for (int i = 0; i < 16 && off + i < 16; ++i)
            if (tb[off + i] != pb[i]) { ok = false; break; }
        if (ok) { irLen_ = off; logInfo("[init] IR length = %d\n", off); return off; }
    }
    logInfo("[init] IR length probe FAILED\n");
    return -1;
}

} // namespace gs32
