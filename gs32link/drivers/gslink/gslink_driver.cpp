/*
 * 层3: gs32link C ABI 对外接口(gs32link_gslink.dll 入口)
 *
 * 分层架构:
 *   层1 ftdi_jtag   — FTDI(FT2232H) D2XX/MPSSE 直驱, JTAG TAP 状态机
 *   层2 riscv_debug — RISC-V Debug 0.13.2 DTM/DMI/DM/SBA, 提供内存读写
 *   层3 本文件      — gs32_* C ABI(句柄/锁/诊断编排)
 */
#include "gslink_log.h"
#include "riscv_debug.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

using gs32::ErrorState;
using gs32::FtdiJtag;
using gs32::RiscvDebug;

struct GsLinkState {
    ErrorState err;
    std::mutex mtx;
    FtdiJtag jtag;
    RiscvDebug dbg;
    bool sbaDiagDone = false;   /* SBA/Abstract 路径探测只做一次 */
    GsLinkState() : dbg(jtag, err) {}
};

std::string g_global_error = "no error";

} // namespace

extern "C" {

GS32LINK_EXPORT const char* gs32_driver_name(void) { return "gslink"; }

GS32LINK_EXPORT int gs32_get_version(char* buf, uint32_t buflen) {
    static const char* v = "gs32link gslink 0.5.0 (D2XX, layered+batch)";
    if (!buf) return (int)strlen(v) + 1;
    uint32_t n = buflen < (uint32_t)strlen(v) + 1 ? buflen : (uint32_t)strlen(v) + 1;
    memcpy(buf, v, n);
    return (int)n;
}

GS32LINK_EXPORT int gs32_get_caps(gs32_handle_t h, uint32_t* caps) {
    if (!caps) return GS32_ERR_PARAM;
    (void)h;
    *caps = GS32_CAP_POLL | GS32_CAP_WRITE;
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_open(const gs32_config_t* cfg, gs32_handle_t* out) {
    if (!cfg || !out) return GS32_ERR_PARAM;
    gs32::debugInit();
    gs32::logInfo("[open] device=%s interface=%s speed=%u\n",
        cfg->device ? cfg->device : "?", cfg->interface ? cfg->interface : "?",
        cfg->speed_khz);
    auto* s = new GsLinkState();
    std::string err;
    if (!s->jtag.open(cfg->speed_khz ? cfg->speed_khz : 4000, err)) {
        g_global_error = err;
        gs32::logInfo("[open] %s\n", err.c_str());
        delete s;
        return GS32_ERR_CONNECT;
    }
    s->err.clear();
    *out = s;
    gs32::logInfo("[open] OK, ready for JTAG\n");
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_close(gs32_handle_t h) {
    auto* s = static_cast<GsLinkState*>(h);
    if (!s) return GS32_ERR_PARAM;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->jtag.close();
    delete s;
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_read(gs32_handle_t h,
                              const gs32_mem_region_t* regions, uint32_t count,
                              uint8_t* out_buf, uint32_t buf_size,
                              uint32_t timeout_ms) {
    (void)timeout_ms;
    auto* s = static_cast<GsLinkState*>(h);
    if (!s || !regions || !count || !out_buf) return GS32_ERR_PARAM;
    std::lock_guard<std::mutex> lk(s->mtx);

    int rc = s->dbg.ensureReady();
    if (rc != GS32_OK) return rc;

    /* 首次读取时做一次 SBA + Abstract 路径探测(用实际监控地址) */
    if (!s->sbaDiagDone) {
        s->dbg.sbaDiag(regions[0].addr);
        s->dbg.abstractDiag(regions[0].addr);
        s->sbaDiagDone = true;
    }

    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i) total += regions[i].size;
    if (buf_size < total) {
        s->err.set(GS32_ERR_PARAM, "out_buf too small");
        return GS32_ERR_PARAM;
    }
    /* 多变量(多 region)整体批量聚合: 单笔 USB 事务读完全部变量,
     * 消除变量间的 ftd2xx 往返间隔 */
    std::vector<gs32::RiscvDebug::MemRegion> regs(count);
    for (uint32_t i = 0; i < count; ++i)
        regs[i] = {regions[i].addr, regions[i].size};
    if (s->dbg.readRegions(regs.data(), count, out_buf)) {
        s->err.set(GS32_ERR_READ, "SBA multi-region read FAILED");
        return GS32_ERR_READ;
    }
    s->err.clear();
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_write(gs32_handle_t h,
                               const gs32_mem_region_t* regions, uint32_t count,
                               const uint8_t* in_buf, uint32_t buf_size,
                               uint32_t timeout_ms) {
    (void)timeout_ms;
    auto* s = static_cast<GsLinkState*>(h);
    if (!s || !regions || !count || !in_buf) return GS32_ERR_PARAM;
    std::lock_guard<std::mutex> lk(s->mtx);

    int rc = s->dbg.ensureReady();
    if (rc != GS32_OK) return rc;

    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i) total += regions[i].size;
    if (buf_size < total) { s->err.set(GS32_ERR_PARAM, "in_buf too small"); return GS32_ERR_PARAM; }

    const uint8_t* p = in_buf;
    for (uint32_t i = 0; i < count; ++i) {
        if (!regions[i].size) continue;
        if (s->dbg.writeMem(regions[i].addr, p, regions[i].size)) {
            char a[16];
            snprintf(a, sizeof(a), "%X", regions[i].addr);
            s->err.set(GS32_ERR_WRITE, "SBA write FAILED @0x" + std::string(a));
            return GS32_ERR_WRITE;
        }
        p += regions[i].size;
    }
    s->err.clear();
    return GS32_OK;
}

GS32LINK_EXPORT const char* gs32_last_error(gs32_handle_t h) {
    return h ? static_cast<GsLinkState*>(h)->err.text() : g_global_error.c_str();
}

/* 诊断统计: 最近一次批量读取的 FT_Write 提交耗时(ms), 采样抖动排查用 */
GS32LINK_EXPORT int gs32_last_write_ms(gs32_handle_t h, double* out_ms) {
    auto* s = static_cast<GsLinkState*>(h);
    if (!s || !out_ms) return GS32_ERR_PARAM;
    std::lock_guard<std::mutex> lk(s->mtx);
    *out_ms = s->dbg.lastWriteMs();
    return GS32_OK;
}

/* STREAM(P1): 无硬件定时采样, 返回未实现(退化为 POLL 模式) */
GS32LINK_EXPORT int gs32_config_stream(gs32_handle_t, uint32_t,
                                       const gs32_mem_region_t*, uint32_t,
                                       const gs32_flush_policy_t*) { return GS32_ERR_NOT_IMPL; }
GS32LINK_EXPORT int gs32_register_notify(gs32_handle_t, gs32_notify_cb, void*) { return GS32_ERR_NOT_IMPL; }
GS32LINK_EXPORT int gs32_start_stream(gs32_handle_t) { return GS32_ERR_NOT_IMPL; }
GS32LINK_EXPORT int gs32_stop_stream(gs32_handle_t)  { return GS32_ERR_NOT_IMPL; }

} /* extern "C" */
