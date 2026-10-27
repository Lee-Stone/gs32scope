/*
 * gs32link_jlink - J-Link 后端
 *
 * 运行时 LoadLibrary("JLinkARM.dll") 动态绑定 SEGGER DLL,
 * 不依赖 SDK 头文件; J-Link 未安装时 open 返回错误并给出提示。
 *
 * 连接语义: gs32_open 完成 DLL 加载与 EMU 打开;
 * 目标附着(Connect)在首次 gs32_read/gs32_write 入口惰性完成。
 */
#include "../common/gs32link_util.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
namespace {

using gs32::ErrorState;

/* ---- SEGGER JLinkARM.dll 原型(仅声明用到的) ---- */
extern "C" {
typedef int  (*PFN_Open)(void);
typedef void (*PFN_Close)(void);
typedef int  (*PFN_Connect)(void);
typedef int  (*PFN_IsOpen)(void);
typedef int  (*PFN_IsConnected)(void);
typedef void (*PFN_SetSpeed)(long);
typedef int  (*PFN_TIF_Select)(int);
typedef int  (*PFN_ReadMemEx)(uint32_t, uint32_t, void*);
typedef int  (*PFN_WriteMemEx)(uint32_t, uint32_t, void*);
typedef int  (*PFN_ExecCommand)(const char*, int, int);
}

struct JLinkApi {
    HMODULE            mod = nullptr;
    PFN_Open           Open = nullptr;
    PFN_Close          Close = nullptr;
    PFN_Connect        Connect = nullptr;
    PFN_IsOpen         IsOpen = nullptr;
    PFN_IsConnected    IsConnected = nullptr;
    PFN_SetSpeed       SetSpeed = nullptr;
    PFN_TIF_Select     TIF_Select = nullptr;
    PFN_ReadMemEx      ReadMemEx = nullptr;
    PFN_WriteMemEx     WriteMemEx = nullptr;
    PFN_ExecCommand    ExecCommand = nullptr;

    /* 按优先级搜索 JLinkARM.dll 的完整路径 */
    static std::string findDll() {
        /* 0) 本驱动 DLL 同目录(绿色分发: 用户把 JLinkARM.dll 拷到 drivers/) */
        {
            HMODULE self = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(&findDll), &self)) {
                char dir[MAX_PATH] = {};
                if (GetModuleFileNameA(self, dir, MAX_PATH)) {
                    std::string p(dir);
                    const size_t s = p.find_last_of("\\/");
                    if (s != std::string::npos) {
                        p = p.substr(0, s + 1) + "JLinkARM.dll";
                        if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                            return p;
                    }
                }
            }
        }
        /* 1) 环境变量 GS32LINK_JLINK_PATH (目录或完整路径) */
        {
            char buf[MAX_PATH] = {};
            const DWORD n = GetEnvironmentVariableA("GS32LINK_JLINK_PATH", buf, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                std::string p(buf);
                if (p.find("JLinkARM.dll") == std::string::npos)
                    p += (p.back() == '\\' || p.back() == '/') ? "JLinkARM.dll" : "\\JLinkARM.dll";
                if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                    return p;
            }
        }
        /* 2) 注册表 InstallPath (HKLM\SOFTWARE\SEGGER\J-Link 与 WOW6432Node) */
        for (const char* key : {"SOFTWARE\\SEGGER\\J-Link",
                                "SOFTWARE\\WOW6432Node\\SEGGER\\J-Link"}) {
            char path[MAX_PATH] = {};
            DWORD size = sizeof(path);
            if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "InstallPath",
                             RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS) {
                std::string p(path);
                if (!p.empty() && p.back() != '\\') p += '\\';
                p += "JLinkARM.dll";
                if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                    return p;
            }
        }
        /* 3) 常见安装路径通配 */
        const char* roots[] = {
            "C:\\Program Files\\SEGGER",
            "C:\\Program Files (x86)\\SEGGER",
            "D:\\SEGGER",
        };
        for (const char* root : roots) {
            WIN32_FIND_DATAA fd{};
            const std::string pat = std::string(root) + "\\JLink*";
            HANDLE h = FindFirstFileA(pat.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            std::string found;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                const std::string p =
                    std::string(root) + "\\" + fd.cFileName + "\\JLinkARM.dll";
                /* 取版本号最大的目录: 简单策略 - 字典序最大 */
                if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES &&
                    p > found)
                    found = p;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
            if (!found.empty()) return found;
        }
        /* 4) 交给系统搜索(PATH) */
        return "JLinkARM.dll";
    }

    bool load(std::string& err) {
        if (mod) return true;
        const std::string dllPath = findDll();
        mod = LoadLibraryA(dllPath.c_str());
        if (!mod) {
            err = "LoadLibrary(" + dllPath + ") failed (err=" +
                  std::to_string(GetLastError()) + "); 请安装 SEGGER J-Link 软件包";
            return false;
        }
        struct Entry { const char* name; void** pfn; };
        const Entry table[] = {
            {"JLINKARM_Open",        (void**)&Open},
            {"JLINKARM_Close",       (void**)&Close},
            {"JLINKARM_Connect",     (void**)&Connect},
            {"JLINKARM_IsOpen",      (void**)&IsOpen},
            {"JLINKARM_IsConnected", (void**)&IsConnected},
            {"JLINKARM_SetSpeed",    (void**)&SetSpeed},
            {"JLINKARM_TIF_Select",  (void**)&TIF_Select},
            {"JLINKARM_ReadMemEx",   (void**)&ReadMemEx},
            {"JLINKARM_WriteMemEx",  (void**)&WriteMemEx},
            {"JLINKARM_ExecCommand", (void**)&ExecCommand},
        };
        std::string missing;
        for (const auto& e : table)
            if (!(*e.pfn = (void*)GetProcAddress(mod, e.name)))
                missing += std::string(e.name) + " ";
        if (!missing.empty()) {
            err = "JLinkARM.dll 缺少符号: " + missing;
            FreeLibrary(mod); mod = nullptr;
            return false;
        }
        return true;
    }
};

struct JLinkState {
    ErrorState err;
    std::mutex mtx;            /* 串行化所有探针访问 */
    JLinkApi api;
    std::string device;
    uint32_t speed_khz = 4000;
    int tif = 1;               /* 0=JTAG 1=SWD */
    std::atomic<bool> connected{false};

    /* 需在持有 mtx 时调用 */
    int ensureConnectedLocked() {
        if (connected.load()) return GS32_OK;
        if (api.ExecCommand && !device.empty()) {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "device = %s", device.c_str());
            if (api.ExecCommand(cmd, 0, 0) < 0) {
                err.set(GS32_ERR_CONNECT, "ExecCommand(device) failed: " + device);
                return GS32_ERR_CONNECT;
            }
        }
        if (api.TIF_Select) api.TIF_Select(tif);
        if (api.SetSpeed)   api.SetSpeed((long)speed_khz);
        if (api.Connect() < 0) {
            err.set(GS32_ERR_CONNECT, "JLINKARM_Connect failed: " + device);
            return GS32_ERR_CONNECT;
        }
        connected.store(true);
        err.clear();
        return GS32_OK;
    }
};

std::string g_global_error = "no error";

} // namespace

extern "C" {

GS32LINK_EXPORT const char* gs32_driver_name(void) { return "jlink"; }

GS32LINK_EXPORT int gs32_get_version(char* buf, uint32_t buflen) {
    static const char* v = "gs32link jlink 0.1.0";
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
    auto* s = new JLinkState();
    std::string err;
    if (!s->api.load(err)) {
        g_global_error = err;
        delete s;
        return GS32_ERR_CONNECT;
    }
    if (cfg->device)    s->device     = cfg->device;
    if (cfg->speed_khz) s->speed_khz  = cfg->speed_khz;
    if (cfg->interface) s->tif        = strcmp(cfg->interface, "jtag") == 0 ? 0 : 1;
    if (!s->api.Open()) {
        g_global_error = "JLINKARM_Open failed";
        delete s;
        return GS32_ERR_CONNECT;
    }
    s->err.clear();
    *out = s;
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_close(gs32_handle_t h) {
    auto* s = static_cast<JLinkState*>(h);
    if (!s) return GS32_ERR_PARAM;
    std::lock_guard<std::mutex> lk(s->mtx);
    if (s->api.mod) {
        if (s->connected.load() && s->api.Close) s->api.Close();
        s->connected.store(false);
    }
    delete s;
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_read(gs32_handle_t h,
                              const gs32_mem_region_t* regions, uint32_t count,
                              uint8_t* out_buf, uint32_t buf_size,
                              uint32_t timeout_ms) {
    (void)timeout_ms;
    auto* s = static_cast<JLinkState*>(h);
    if (!s || !regions || !count || !out_buf) return GS32_ERR_PARAM;
    std::lock_guard<std::mutex> lk(s->mtx);

    int rc = s->ensureConnectedLocked();
    if (rc != GS32_OK) return rc;

    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i) total += regions[i].size;
    if (buf_size < total) {
        s->err.set(GS32_ERR_PARAM, "out_buf too small");
        return GS32_ERR_PARAM;
    }

    uint8_t* p = out_buf;
    for (uint32_t i = 0; i < count; ++i) {
        if (!regions[i].size) continue;
        if (s->api.ReadMemEx(regions[i].addr, regions[i].size, p) < 0) {
            char a[16];
            snprintf(a, sizeof(a), "%08X", regions[i].addr);
            s->err.set(GS32_ERR_READ, std::string("ReadMemEx failed @0x") + a);
            return GS32_ERR_READ;
        }
        p += regions[i].size;
    }
    s->err.clear();
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_write(gs32_handle_t h,
                               const gs32_mem_region_t* regions, uint32_t count,
                               const uint8_t* in_buf, uint32_t buf_size,
                               uint32_t timeout_ms) {
    (void)timeout_ms;
    auto* s = static_cast<JLinkState*>(h);
    if (!s || !regions || !count || !in_buf) return GS32_ERR_PARAM;
    std::lock_guard<std::mutex> lk(s->mtx);

    int rc = s->ensureConnectedLocked();
    if (rc != GS32_OK) return rc;

    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i) total += regions[i].size;
    if (buf_size < total) { s->err.set(GS32_ERR_PARAM, "in_buf too small"); return GS32_ERR_PARAM; }

    const uint8_t* p = in_buf;
    for (uint32_t i = 0; i < count; ++i) {
        if (!regions[i].size) continue;
        if (s->api.WriteMemEx(regions[i].addr, regions[i].size, (void*)p) < 0) {
            s->err.set(GS32_ERR_WRITE, "WriteMemEx failed");
            return GS32_ERR_WRITE;
        }
        p += regions[i].size;
    }
    s->err.clear();
    return GS32_OK;
}

GS32LINK_EXPORT const char* gs32_last_error(gs32_handle_t h) {
    return h ? static_cast<JLinkState*>(h)->err.text() : g_global_error.c_str();
}

/* STREAM(P1): J-Link 无硬件定时采样, 返回未实现(退化为 POLL 模式) */
GS32LINK_EXPORT int gs32_config_stream(gs32_handle_t, uint32_t,
                                       const gs32_mem_region_t*, uint32_t,
                                       const gs32_flush_policy_t*) { return GS32_ERR_NOT_IMPL; }
GS32LINK_EXPORT int gs32_register_notify(gs32_handle_t, gs32_notify_cb, void*) { return GS32_ERR_NOT_IMPL; }
GS32LINK_EXPORT int gs32_start_stream(gs32_handle_t) { return GS32_ERR_NOT_IMPL; }
GS32LINK_EXPORT int gs32_stop_stream(gs32_handle_t)  { return GS32_ERR_NOT_IMPL; }

} /* extern "C" */
