/*
 * gs32link_sci / gs32link_can - SCI/UART 或 CAN 网关通讯后端
 *
 * 目标侧协议(小端):
 *   Request : A5 5A | cmd | seq | addr(4) | len(2) | payload(len) | crc16(2)
 *   Response: 5A A5 | cmd | seq | status | len(2) | payload(len) | crc16(2)
 *   cmd: 0x01 read (payload 为空, len=要读字节数)
 *        0x02 write (payload=数据, len=字节数)
 *   crc16: CCITT-FALSE, poly 0x1021, init 0xFFFF, 覆盖 cmd..payload
 *   status: 0=OK, 1=CRC, 2=参数, 3=未知命令
 *
 * extra JSON:
 *   {"transport":"tcp","host":"127.0.0.1","port":7000}
 *   {"transport":"serial","com":"COM3","baud":115200}
 *
 * SCI: 直连串口; 同一份源码用 GS32LINK_DRIVER_CAN 编译出 can 后端,
 *      默认 TCP 7001, 面向"CAN <-> Ethernet"网关(网关把 CAN 帧转换成同一字节流)。
 *      真实 USB-CAN 适配器需要提供等价网关/固件。
 */
#include "../common/gs32link_util.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef GS32LINK_DRIVER_CAN
#define GS32LINK_DRIVER_NAME "can"
#define GS32LINK_DEFAULT_PORT 7001
#else
#define GS32LINK_DRIVER_NAME "sci"
#define GS32LINK_DEFAULT_PORT 7000
#endif

namespace {

using gs32::ErrorState;

constexpr uint8_t kReq0 = 0xA5, kReq1 = 0x5A;
constexpr uint8_t kRsp0 = 0x5A, kRsp1 = 0xA5;
constexpr uint8_t kCmdRead = 0x01, kCmdWrite = 0x02;
constexpr uint16_t kMaxPayload = 256;

struct SciState {
    ErrorState err;
    std::mutex mtx;
    bool tcp = false;
    SOCKET sock = INVALID_SOCKET;
    HANDLE com = INVALID_HANDLE_VALUE;
    uint8_t seq = 0;
    std::string host = "127.0.0.1";
    int port = GS32LINK_DEFAULT_PORT;
    std::string comName = "COM3";
    int baud = 115200;
};

std::string g_global_error = "no error";

uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= uint16_t(d[i]) << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return crc;
}

std::string jsonStr(const char* json, const char* key) {
    if (!json) return {};
    /* 必须匹配带引号的完整键, 否则 "port" 会命中 "transport" 里的 port */
    const std::string pattern = std::string("\"") + key + "\"";
    const char* p = std::strstr(json, pattern.c_str());
    if (!p) return {};
    p = std::strchr(p + pattern.size(), ':');
    if (!p) return {};
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '"') {
        ++p;
        const char* e = std::strchr(p, '"');
        return e ? std::string(p, e - p) : std::string();
    }
    const char* e = p;
    while (*e && *e != ',' && *e != '}' && *e != ' ' && *e != '\r' && *e != '\n') ++e;
    return std::string(p, e - p);
}

bool connectTcp(SciState* s) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        s->err.set(GS32_ERR_CONNECT, "WSAStartup 失败");
        return false;
    }
    SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        s->err.set(GS32_ERR_CONNECT, "创建 TCP socket 失败");
        WSACleanup();
        return false;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(s->port));
    if (inet_pton(AF_INET, s->host.c_str(), &a.sin_addr) != 1) {
        s->err.set(GS32_ERR_CONNECT, "目标地址非法: " + s->host);
        closesocket(sock); WSACleanup();
        return false;
    }
    if (::connect(sock, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR) {
        s->err.set(GS32_ERR_CONNECT, "连接 " + s->host + ":" + std::to_string(s->port) + " 失败");
        closesocket(sock); WSACleanup();
        return false;
    }
    DWORD timeout = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    s->sock = sock;
    s->tcp = true;
    return true;
}

bool connectSerial(SciState* s) {
    const std::string path = "\\\\.\\" + s->comName;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        s->err.set(GS32_ERR_CONNECT, "打开串口失败: " + s->comName);
        return false;
    }
    SetupComm(h, 4096, 4096);
    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(h, &dcb)) {
        s->err.set(GS32_ERR_CONNECT, "读取串口参数失败");
        CloseHandle(h);
        return false;
    }
    dcb.BaudRate = DWORD(s->baud);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) {
        s->err.set(GS32_ERR_CONNECT, "设置串口参数失败: " + s->comName);
        CloseHandle(h);
        return false;
    }
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = 50;
    to.ReadTotalTimeoutConstant = 500;
    to.ReadTotalTimeoutMultiplier = 10;
    to.WriteTotalTimeoutConstant = 500;
    to.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    s->com = h;
    s->tcp = false;
    return true;
}

bool writeAll(SciState* s, const uint8_t* d, size_t n) {
    if (s->tcp) {
        size_t off = 0;
        while (off < n) {
            const int r = ::send(s->sock, reinterpret_cast<const char*>(d + off), int(n - off), 0);
            if (r <= 0) { s->err.set(GS32_ERR_WRITE, "TCP 发送失败"); return false; }
            off += size_t(r);
        }
        return true;
    }
    size_t off = 0;
    while (off < n) {
        DWORD written = 0;
        if (!WriteFile(s->com, d + off, DWORD(n - off), &written, nullptr) || written == 0) {
            s->err.set(GS32_ERR_WRITE, "串口写失败");
            return false;
        }
        off += written;
    }
    return true;
}

bool readExact(SciState* s, uint8_t* d, size_t n) {
    if (s->tcp) {
        size_t off = 0;
        while (off < n) {
            const int r = ::recv(s->sock, reinterpret_cast<char*>(d + off), int(n - off), 0);
            if (r <= 0) { s->err.set(GS32_ERR_TIMEOUT, "TCP 接收失败/超时"); return false; }
            off += size_t(r);
        }
        return true;
    }
    size_t off = 0;
    while (off < n) {
        DWORD got = 0;
        if (!ReadFile(s->com, d + off, DWORD(n - off), &got, nullptr)) {
            s->err.set(GS32_ERR_READ, "串口读失败");
            return false;
        }
        if (got == 0) { s->err.set(GS32_ERR_TIMEOUT, "串口读超时"); return false; }
        off += got;
    }
    return true;
}

/* 调用方持有 mtx */
bool xferLocked(SciState* s, uint8_t cmd, uint32_t addr, uint16_t len,
                const uint8_t* in, uint8_t* out) {
    std::vector<uint8_t> req;
    req.reserve(10 + len + 2);
    req.push_back(kReq0);
    req.push_back(kReq1);
    req.push_back(cmd);
    const uint8_t seq = s->seq++;
    req.push_back(seq);
    for (int i = 0; i < 4; ++i) req.push_back(uint8_t((addr >> (8 * i)) & 0xFF));
    req.push_back(uint8_t(len & 0xFF));
    req.push_back(uint8_t((len >> 8) & 0xFF));
    if (in) req.insert(req.end(), in, in + len);
    const uint16_t crc = crc16(req.data() + 2, req.size() - 2);
    req.push_back(uint8_t(crc & 0xFF));
    req.push_back(uint8_t(crc >> 8));
    if (!writeAll(s, req.data(), req.size())) return false;

    uint8_t rh[7] = {};
    if (!readExact(s, rh, sizeof(rh))) return false;
    if (rh[0] != kRsp0 || rh[1] != kRsp1) {
        s->err.set(GS32_ERR_READ, "响应帧头错误(检查 SCI 目标固件/波特率)");
        return false;
    }
    const uint8_t rcmd = rh[2], rseq = rh[3], status = rh[4];
    const uint16_t rlen = uint16_t(rh[5] | (rh[6] << 8));
    if (rcmd != cmd || rseq != seq) {
        s->err.set(GS32_ERR_READ, "响应命令/序号不匹配");
        return false;
    }
    std::vector<uint8_t> rp(rlen);
    if (rlen && !readExact(s, rp.data(), rlen)) return false;
    uint8_t crcb[2] = {};
    if (!readExact(s, crcb, 2)) return false;
    std::vector<uint8_t> crcBuf(rh + 2, rh + 7);
    crcBuf.insert(crcBuf.end(), rp.begin(), rp.end());
    const uint16_t want = crc16(crcBuf.data(), crcBuf.size());
    const uint16_t got = uint16_t(crcb[0] | (crcb[1] << 8));
    if (want != got) {
        s->err.set(GS32_ERR_READ, "响应 CRC 校验失败");
        return false;
    }
    if (status != 0) {
        s->err.set(cmd == kCmdRead ? GS32_ERR_READ : GS32_ERR_WRITE,
                   "目标返回状态 " + std::to_string(status));
        return false;
    }
    if (cmd == kCmdRead) {
        if (rlen != len || !out) {
            s->err.set(GS32_ERR_READ, "读响应长度不符");
            return false;
        }
        std::memcpy(out, rp.data(), rlen);
    }
    return true;
}

} // namespace

extern "C" {

GS32LINK_EXPORT const char* gs32_driver_name(void) { return GS32LINK_DRIVER_NAME; }

GS32LINK_EXPORT int gs32_get_version(char* buf, uint32_t buflen) {
    static const char* v = "gs32link " GS32LINK_DRIVER_NAME " 0.1.0";
    if (!buf) return int(std::strlen(v)) + 1;
    const uint32_t n = buflen < uint32_t(std::strlen(v)) + 1 ? buflen : uint32_t(std::strlen(v)) + 1;
    std::memcpy(buf, v, n);
    return int(n);
}

GS32LINK_EXPORT int gs32_get_caps(gs32_handle_t, uint32_t* caps) {
    if (!caps) return GS32_ERR_PARAM;
    *caps = GS32_CAP_POLL | GS32_CAP_WRITE;
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_open(const gs32_config_t* cfg, gs32_handle_t* out) {
    if (!cfg || !out) return GS32_ERR_PARAM;
    auto* s = new SciState();
    if (cfg->extra) {
        const std::string transport = jsonStr(cfg->extra, "transport");
        const std::string host = jsonStr(cfg->extra, "host");
        const std::string port = jsonStr(cfg->extra, "port");
        const std::string com = jsonStr(cfg->extra, "com");
        const std::string baud = jsonStr(cfg->extra, "baud");
        if (!host.empty()) s->host = host;
        if (!port.empty()) s->port = std::atoi(port.c_str());
        if (!com.empty()) s->comName = com;
        if (!baud.empty()) s->baud = std::atoi(baud.c_str());
        s->tcp = (transport == "tcp") || (transport.empty() && !host.empty());
    }
    const bool ok = s->tcp ? connectTcp(s) : connectSerial(s);
    if (!ok) {
        g_global_error = s->err.text();
        delete s;
        return GS32_ERR_CONNECT;
    }
    s->err.clear();
    g_global_error = "no error";
    *out = s;
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_close(gs32_handle_t h) {
    auto* s = static_cast<SciState*>(h);
    if (!s) return GS32_OK;
    if (s->tcp && s->sock != INVALID_SOCKET) {
        closesocket(s->sock);
        s->sock = INVALID_SOCKET;
        WSACleanup();
    }
    if (!s->tcp && s->com != INVALID_HANDLE_VALUE) {
        CloseHandle(s->com);
        s->com = INVALID_HANDLE_VALUE;
    }
    delete s;
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_read(gs32_handle_t h,
                              const gs32_mem_region_t* regions, uint32_t count,
                              uint8_t* out_buf, uint32_t buf_size,
                              uint32_t timeout_ms) {
    (void)timeout_ms;
    auto* s = static_cast<SciState*>(h);
    if (!s || !regions || !count || !out_buf) return GS32_ERR_PARAM;
    std::lock_guard<std::mutex> lk(s->mtx);
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i) total += regions[i].size;
    if (buf_size < total) {
        s->err.set(GS32_ERR_PARAM, "out_buf too small");
        return GS32_ERR_PARAM;
    }
    uint8_t* p = out_buf;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t addr = regions[i].addr;
        uint32_t left = regions[i].size;
        while (left) {
            const uint16_t chunk = uint16_t(left > kMaxPayload ? kMaxPayload : left);
            if (!xferLocked(s, kCmdRead, addr, chunk, nullptr, p)) return s->err.code();
            addr += chunk;
            p += chunk;
            left -= chunk;
        }
    }
    s->err.clear();
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_write(gs32_handle_t h,
                               const gs32_mem_region_t* regions, uint32_t count,
                               const uint8_t* in_buf, uint32_t buf_size,
                               uint32_t timeout_ms) {
    (void)timeout_ms;
    auto* s = static_cast<SciState*>(h);
    if (!s || !regions || !count || !in_buf) return GS32_ERR_PARAM;
    std::lock_guard<std::mutex> lk(s->mtx);
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; ++i) total += regions[i].size;
    if (buf_size < total) {
        s->err.set(GS32_ERR_PARAM, "in_buf too small");
        return GS32_ERR_PARAM;
    }
    const uint8_t* p = in_buf;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t addr = regions[i].addr;
        uint32_t left = regions[i].size;
        while (left) {
            const uint16_t chunk = uint16_t(left > kMaxPayload ? kMaxPayload : left);
            if (!xferLocked(s, kCmdWrite, addr, chunk, p, nullptr)) return s->err.code();
            addr += chunk;
            p += chunk;
            left -= chunk;
        }
    }
    s->err.clear();
    return GS32_OK;
}

GS32LINK_EXPORT const char* gs32_last_error(gs32_handle_t h) {
    return h ? static_cast<SciState*>(h)->err.text() : g_global_error.c_str();
}

GS32LINK_EXPORT int gs32_config_stream(gs32_handle_t, uint32_t,
                                       const gs32_mem_region_t*, uint32_t,
                                       const gs32_flush_policy_t*) { return GS32_ERR_NOT_IMPL; }
GS32LINK_EXPORT int gs32_register_notify(gs32_handle_t, gs32_notify_cb, void*) { return GS32_ERR_NOT_IMPL; }
GS32LINK_EXPORT int gs32_start_stream(gs32_handle_t) { return GS32_ERR_NOT_IMPL; }
GS32LINK_EXPORT int gs32_stop_stream(gs32_handle_t)  { return GS32_ERR_NOT_IMPL; }

} /* extern "C" */
