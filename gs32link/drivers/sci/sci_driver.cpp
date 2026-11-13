/*
 * gs32link_sci / gs32link_can - FreeMASTER 兼容的串口/CAN 网关通讯后端
 *
 * 线上协议遵循 NXP FreeMASTER serial transport + protocol v4:
 *   Frame : SOB(0x2B) | body... | CRC8
 *   body  : cmd | len | payload[len]        (len = payload 字节数, 不含 CRC)
 *           payload 中的每个 0x2B 字节按 FreeMASTER 规则重复一次
 *   CRC8  : poly 0x07, init 0x00, MSB first, 覆盖 cmd..payload
 *
 *   命令:
 *     0x21 READMEM  : payload = ULEB(addr) + ULEB(size)
 *     0x23 WRITEMEM : payload = flags(1) + ULEB(addr) + ULEB(size) + data
 *     0x20 GETCONFIG: 保留(未使用)
 *
 *   响应:
 *     OK 且带数据 : status = 0x40 (FMSTR_STSF_VARLEN) | 0x00, 后跟 len + data
 *     OK 无数据   : status = 0x00
 *     错误        : status = 0x80 | 错误码, 无数据
 *
 *   地址/长度按 FreeMASTER FMSTR_UlebEncode 规则使用 ULEB128 编码(小端 7bit 组)。
 *
 * extra JSON:
 *   {"transport":"tcp","host":"127.0.0.1","port":7000}   (测试/CAN 网关)
 *   {"transport":"serial","com":"COM3","baud":115200}
 *
 * SCI: 直连串口; 同一份源码用 GS32LINK_DRIVER_CAN 编译出 can 后端,
 *      默认 TCP 7001, 面向"CAN <-> Ethernet"网关(网关还原成同一字节流)。
 */
#include "../common/gs32link_util.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
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

/* ---- FreeMASTER 协议常量 (freemaster_protocol.h) ---- */
constexpr uint8_t kSob = 0x2B;              /* FMSTR_SOB '+' */
constexpr uint8_t kCmdGetConfig = 0x20;
constexpr uint8_t kCmdReadMem   = 0x21;
constexpr uint8_t kCmdWriteMem  = 0x23;
/* GS32Scope STREAM */
constexpr uint8_t kCmdStreamCfg   = 0x40;
constexpr uint8_t kCmdStreamStart = 0x41;
constexpr uint8_t kCmdStreamStop  = 0x42;
constexpr uint8_t kCmdStreamRead  = 0x44;
constexpr uint8_t kCmdStreamReadN = 0x45;   /* burst: 1 request, N responses */
constexpr uint8_t kStsOk        = 0x00;
constexpr uint8_t kStsError     = 0x80;     /* FMSTR_STSF_ERROR */
constexpr uint8_t kStsVarLen    = 0x40;     /* FMSTR_STSF_VARLEN */
constexpr uint8_t kWrmFlagNone  = 0x00;

/* FreeMASTER 串口响应上限 252 字节(含 status/len), 请求侧保守取 200/128 */
constexpr size_t kMaxReadChunk  = 200;
/* STREAM 每帧上限: 240 数据 + 2 个 ULEB 头, 不超过响应 252 字节 */
constexpr uint32_t kStreamMaxBytes = 244;
constexpr size_t kMaxWriteChunk = 128;
constexpr size_t kRxBufSize     = 1024;

struct SciState {
    ErrorState err;
    std::mutex mtx;
    bool tcp = false;
    SOCKET sock = INVALID_SOCKET;
    HANDLE com = INVALID_HANDLE_VALUE;
    std::string host = "127.0.0.1";
    int port = GS32LINK_DEFAULT_PORT;
    std::string comName = "COM3";
    int baud = 115200;
    uint32_t ioTimeoutMs = 200;
    std::vector<uint8_t> rx;
    size_t rxPos = 0;

    /* STREAM state */
    struct StreamState {
        bool configured = false;
        std::atomic<bool> running{false};
        uint32_t periodNs = 0;
        uint32_t ringAddr = 0;
        uint32_t ringBytes = 0;
        uint32_t sampleBytes = 0;
        uint32_t capacity = 0;
        uint32_t regionCount = 0;
        uint32_t maxSamples = 22;
        gs32_notify_cb cb = nullptr;
        void* user = nullptr;
        std::thread th;
        uint32_t readSeq = 0;
        uint64_t seq = 0;
        int64_t t0Ns = 0;
        /* 0x45 burst: 一次请求读多帧, 摊薄 VCP 16ms 延迟 */
        uint32_t burst = 3;
        bool useBurst = true;
        int burstFails = 0;
    } stream;
};

std::string g_global_error = "no error";

/* FreeMASTER CRC8: poly 0x07, init 0x00, MSB first */
uint8_t crc8(const uint8_t* d, size_t n) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x80) ? uint8_t((crc << 1) ^ 0x07) : uint8_t(crc << 1);
    }
    return crc;
}

/* FreeMASTER FMSTR_UlebEncode: 7bit/字节, 高位置 1 表示后续还有 */
void ulebEncode(std::vector<uint8_t>& out, uint32_t v) {
    do {
        uint8_t b = uint8_t(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        out.push_back(b);
    } while (v);
}

bool ulebDecode(const std::vector<uint8_t>& buf, size_t* pos, uint32_t* out) {
    uint32_t v = 0U;
    int shift = 0;
    while (*pos < buf.size() && shift <= 28) {
        const uint8_t b = buf[(*pos)++];
        v |= uint32_t(b & 0x7FU) << shift;
        if ((b & 0x80U) == 0U) {
            *out = v;
            return true;
        }
        shift += 7;
    }
    return false;
}

std::string jsonStr(const char* json, const char* key) {
    if (!json) return {};
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
    /* 收到第一个字节就立即返回, 不要等 ReadInterval/Total 超时:
     * FTDI VCP 的 16ms latency timer 已经够慢, 再叠加超时会把每次事务拖到 32ms。
     * MAXDWORD 组合 = "有数据立即返回"; Constant 只作为首字节的最长等待。 */
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = 100;
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

/* 预读缓冲: 一次系统调用尽量取回多字节, 供逐字节状态机消费 */
bool fillRx(SciState* s) {
    if (s->rxPos < s->rx.size()) return true;
    s->rx.resize(kRxBufSize);
    s->rxPos = 0;
    if (s->tcp) {
        const int r = ::recv(s->sock, reinterpret_cast<char*>(s->rx.data()), int(s->rx.size()), 0);
        if (r <= 0) { s->err.set(GS32_ERR_TIMEOUT, "TCP 接收失败/超时"); return false; }
        s->rx.resize(size_t(r));
        return true;
    }
    DWORD got = 0;
    if (!ReadFile(s->com, s->rx.data(), DWORD(s->rx.size()), &got, nullptr)) {
        s->err.set(GS32_ERR_READ, "串口读失败");
        return false;
    }
    if (got == 0) {
        s->err.set(GS32_ERR_TIMEOUT, "串口读超时");
        return false;
    }
    s->rx.resize(got);
    return true;
}

bool nextByte(SciState* s, uint8_t* out) {
    if (!fillRx(s)) return false;
    *out = s->rx[s->rxPos++];
    return true;
}

/* 读一个"已解转义"的帧内字节: 0x2B 0x2B -> 0x2B */
bool nextUnescaped(SciState* s, uint8_t* out) {
    uint8_t b = 0;
    if (!nextByte(s, &b)) return false;
    if (b != kSob) { *out = b; return true; }
    uint8_t b2 = 0;
    if (!nextByte(s, &b2)) return false;
    if (b2 != kSob) {
        /* 帧内出现单个 SOB: 协议错误, 按原字节返回让上层判 CRC 失败 */
        *out = b;
        s->rxPos--;   /* 把 b2 留给下一字节 */
        return true;
    }
    *out = kSob;
    return true;
}

/* 组帧: SOB + (cmd,len,payload,crc), 其中 0x2B 重复一次 */
void buildFrame(uint8_t cmd, const std::vector<uint8_t>& payload, std::vector<uint8_t>* wire) {
    std::vector<uint8_t> body;
    body.reserve(payload.size() + 3);
    body.push_back(cmd);
    body.push_back(uint8_t(payload.size()));
    body.insert(body.end(), payload.begin(), payload.end());
    const uint8_t c = crc8(body.data(), body.size());
    body.push_back(c);

    wire->clear();
    wire->reserve(body.size() + 4);
    wire->push_back(kSob);
    for (uint8_t b : body) {
        wire->push_back(b);
        if (b == kSob) wire->push_back(b);
    }
}

/* 等待并解析一帧响应。data 返回 OK 且 VARLEN 时的负载 */
bool readFrame(SciState* s, uint8_t* status, std::vector<uint8_t>* data) {
    /* 找帧头: SOB 后面必须跟一个非 SOB 的状态字节 */
    bool found = false;
    for (int tries = 0; tries < 256; ++tries) {
        uint8_t b = 0;
        if (!nextByte(s, &b)) return false;
        if (b != kSob) continue;
        uint8_t st = 0;
        if (!nextByte(s, &st)) return false;
        if (st == kSob) continue;   /* 0x2B 0x2B: 传输中的字面 SOB, 继续找 */
        *status = st;
        found = true;
        break;
    }
    if (!found) {
        s->err.set(GS32_ERR_READ, "未收到 FreeMASTER 响应帧头");
        return false;
    }

    std::vector<uint8_t> body;
    body.push_back(*status);

    size_t need = 0;
    if ((*status & kStsVarLen) != 0) {
        uint8_t len = 0;
        if (!nextUnescaped(s, &len)) return false;
        body.push_back(len);
        need = len;
    }
    data->clear();
    data->reserve(need);
    for (size_t i = 0; i < need; ++i) {
        uint8_t b = 0;
        if (!nextUnescaped(s, &b)) return false;
        body.push_back(b);
        data->push_back(b);
    }
    uint8_t got = 0;
    if (!nextUnescaped(s, &got)) return false;
    const uint8_t want = crc8(body.data(), body.size());
    if (got != want) {
        s->err.set(GS32_ERR_READ, "响应 CRC8 校验失败");
        return false;
    }
    return true;
}

/* 调用方持有 mtx */
bool transactLocked(SciState* s, uint8_t cmd, const std::vector<uint8_t>& payload,
                    uint8_t* status, std::vector<uint8_t>* data) {
    std::vector<uint8_t> wire;
    buildFrame(cmd, payload, &wire);
    s->rx.clear();
    s->rxPos = 0;
    if (!writeAll(s, wire.data(), wire.size())) return false;
    if (!readFrame(s, status, data)) return false;

    if ((*status & kStsError) != 0) {
        s->err.set(cmd == kCmdReadMem ? GS32_ERR_READ : GS32_ERR_WRITE,
                   "目标返回 FreeMASTER 错误码 0x" + std::to_string(unsigned(*status)));
        return false;
    }
    if (*status != kStsOk && *status != (kStsOk | kStsVarLen)) {
        s->err.set(GS32_ERR_READ,
                   "目标返回未知状态 0x" + std::to_string(unsigned(*status)));
        return false;
    }
    return true;
}

bool readMemory(SciState* s, uint32_t addr, uint32_t size, uint8_t* out) {
    std::vector<uint8_t> payload;
    ulebEncode(payload, addr);
    ulebEncode(payload, size);
    uint8_t status = 0;
    std::vector<uint8_t> data;
    if (!transactLocked(s, kCmdReadMem, payload, &status, &data)) return false;
    if (data.size() != size) {
        s->err.set(GS32_ERR_READ, "读响应长度不符: 期望 " + std::to_string(size) +
                                      ", 实际 " + std::to_string(data.size()));
        return false;
    }
    std::memcpy(out, data.data(), size);
    return true;
}

bool writeMemory(SciState* s, uint32_t addr, uint32_t size, const uint8_t* in) {
    std::vector<uint8_t> payload;
    payload.push_back(kWrmFlagNone);
    ulebEncode(payload, addr);
    ulebEncode(payload, size);
    payload.insert(payload.end(), in, in + size);
    uint8_t status = 0;
    std::vector<uint8_t> data;
    if (!transactLocked(s, kCmdWriteMem, payload, &status, &data)) return false;
    if (!data.empty()) {
        s->err.set(GS32_ERR_WRITE, "写响应不应携带数据");
        return false;
    }
    return true;
}

/* One STREAM_READ transaction returns the write pointer plus new samples. */
bool streamReadLocked(SciState* s, uint32_t readSeq, uint32_t maxBytes,
                      uint32_t* writeSeq, uint32_t* dropped,
                      std::vector<uint8_t>* data) {
    std::vector<uint8_t> payload;
    ulebEncode(payload, readSeq);
    ulebEncode(payload, maxBytes);
    uint8_t status = 0;
    std::vector<uint8_t> resp;
    if (!transactLocked(s, kCmdStreamRead, payload, &status, &resp)) return false;
    size_t pos = 0U;
    if (!ulebDecode(resp, &pos, writeSeq) || !ulebDecode(resp, &pos, dropped)) {
        s->err.set(GS32_ERR_READ, "STREAM 响应头无效");
        return false;
    }
    data->assign(resp.begin() + std::ptrdiff_t(pos), resp.end());
    return true;
}

/* Burst STREAM_READ (0x45): one request answered with `count` frames.
 * Each frame carries writeSeq, dropped and raw samples, back to back. */
bool streamReadNLocked(SciState* s, uint32_t readSeq, uint32_t maxBytes, uint32_t count,
                       uint32_t* firstWriteSeq, uint32_t* firstDropped,
                       uint32_t* totalDropped, std::vector<uint8_t>* data) {
    std::vector<uint8_t> payload;
    ulebEncode(payload, readSeq);
    ulebEncode(payload, maxBytes);
    ulebEncode(payload, count);

    std::vector<uint8_t> wire;
    buildFrame(kCmdStreamReadN, payload, &wire);
    s->rx.clear();
    s->rxPos = 0;
    if (!writeAll(s, wire.data(), wire.size())) return false;

    data->clear();
    *totalDropped = 0U;
    *firstWriteSeq = 0U;
    *firstDropped = 0U;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t status = 0;
        std::vector<uint8_t> resp;
        if (!readFrame(s, &status, &resp)) return false;
        if ((status & kStsError) != 0) {
            s->err.set(GS32_ERR_READ, "STREAM 读取返回错误码 0x" +
                                          std::to_string(unsigned(status)));
            return false;
        }
        if (status != (kStsOk | kStsVarLen)) {
            s->err.set(GS32_ERR_READ, "STREAM 响应状态异常 0x" +
                                          std::to_string(unsigned(status)));
            return false;
        }
        size_t pos = 0U;
        uint32_t w = 0U;
        uint32_t dropped = 0U;
        if (!ulebDecode(resp, &pos, &w) || !ulebDecode(resp, &pos, &dropped)) {
            s->err.set(GS32_ERR_READ, "STREAM 响应头无效");
            return false;
        }
        if (i == 0U) {
            *firstWriteSeq = w;
            *firstDropped = dropped;
        }
        *totalDropped += dropped;
        data->insert(data->end(), resp.begin() + std::ptrdiff_t(pos), resp.end());
    }
    return true;
}

void streamThread(SciState* s) {
    using namespace std::chrono;
    s->stream.t0Ns = duration_cast<nanoseconds>(
                         steady_clock::now().time_since_epoch()).count();

    while (s->stream.running.load()) {
        uint32_t writeSeq = 0U;
        uint32_t firstDropped = 0U;
        uint32_t totalDropped = 0U;
        std::vector<uint8_t> data;
        bool ok;
        const bool burst = s->stream.useBurst;
        {
            std::lock_guard<std::mutex> lk(s->mtx);
            if (burst) {
                ok = streamReadNLocked(s, s->stream.readSeq, kStreamMaxBytes,
                                       s->stream.burst, &writeSeq, &firstDropped,
                                       &totalDropped, &data);
            } else {
                ok = streamReadLocked(s, s->stream.readSeq, kStreamMaxBytes,
                                      &writeSeq, &firstDropped, &data);
                totalDropped = firstDropped;
            }
        }
        if (!ok) {
            /* 目标端不支持 0x45 时自动退回单帧读取(旧固件兼容) */
            if (burst && ++s->stream.burstFails >= 2) {
                s->stream.useBurst = false;
            }
            std::this_thread::sleep_for(milliseconds(5));
            continue;
        }
        s->stream.burstFails = 0;

        const uint32_t sampleBytes = s->stream.sampleBytes;
        if (sampleBytes == 0U) break;
        const uint32_t samples = uint32_t(data.size()) / sampleBytes;
        /* 目标端返回的第一个样本是 readSeq + firstDropped, 时间戳必须跟着
         * 挪, 否则丢样时整块时间提前, 与上一块重叠被画成竖线. */
        const uint64_t t0 = uint64_t(s->stream.t0Ns) +
                            uint64_t(s->stream.readSeq + firstDropped) *
                                uint64_t(s->stream.periodNs);
        s->stream.readSeq += totalDropped + samples;

        if (samples == 0U) {
            std::this_thread::sleep_for(milliseconds(2));
            continue;
        }

        gs32_block_hdr_t hdr{};
        hdr.magic = GS32_BLOCK_MAGIC;
        hdr.version = GS32_BLOCK_VERSION;
        hdr.header_size = sizeof(gs32_block_hdr_t);
        hdr.seq = s->stream.seq++;
        hdr.t0_ns = t0;
        hdr.period_ns = s->stream.periodNs;
        hdr.sample_count = uint16_t(samples);
        hdr.region_count = uint16_t(s->stream.regionCount);
        hdr.flags = (totalDropped != 0U) ? GS32_BF_TRUNCATED : 0U;
        hdr.payload_size = uint32_t(data.size());

        if (s->stream.cb) {
            s->stream.cb(reinterpret_cast<gs32_handle_t>(s), &hdr, data.data(), s->stream.user);
        }
    }
}

} // namespace

extern "C" {

GS32LINK_EXPORT const char* gs32_driver_name(void) { return GS32LINK_DRIVER_NAME; }

GS32LINK_EXPORT int gs32_get_version(char* buf, uint32_t buflen) {
    static const char* v = "gs32link " GS32LINK_DRIVER_NAME " 0.2.0 (freemaster)";
    if (!buf) return int(std::strlen(v)) + 1;
    const uint32_t n = buflen < uint32_t(std::strlen(v)) + 1 ? buflen : uint32_t(std::strlen(v)) + 1;
    std::memcpy(buf, v, n);
    return int(n);
}

GS32LINK_EXPORT int gs32_get_caps(gs32_handle_t, uint32_t* caps) {
    if (!caps) return GS32_ERR_PARAM;
    *caps = GS32_CAP_POLL | GS32_CAP_WRITE | GS32_CAP_STREAM;
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
    s->stream.running.store(false);
    if (s->stream.th.joinable()) s->stream.th.join();
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
            const uint32_t chunk = left > kMaxReadChunk ? uint32_t(kMaxReadChunk) : left;
            if (!readMemory(s, addr, chunk, p)) return s->err.code();
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
            const uint32_t chunk = left > kMaxWriteChunk ? uint32_t(kMaxWriteChunk) : left;
            if (!writeMemory(s, addr, chunk, p)) return s->err.code();
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

GS32LINK_EXPORT int gs32_config_stream(gs32_handle_t h, uint32_t period_ns,
                                       const gs32_mem_region_t* regions, uint32_t count,
                                       const gs32_flush_policy_t* policy) {
    auto* s = static_cast<SciState*>(h);
    if (!s || !regions || count == 0U || period_ns == 0U) return GS32_ERR_PARAM;
    std::lock_guard<std::mutex> lk(s->mtx);
    if (s->stream.running.load()) return GS32_ERR_STATE;

    std::vector<uint8_t> payload;
    ulebEncode(payload, period_ns);
    ulebEncode(payload, count);
    for (uint32_t i = 0; i < count; ++i) {
        ulebEncode(payload, regions[i].addr);
        ulebEncode(payload, regions[i].size);
    }
    uint8_t status = 0;
    std::vector<uint8_t> data;
    if (!transactLocked(s, kCmdStreamCfg, payload, &status, &data)) return s->err.code();

    size_t pos = 0U;
    uint32_t addr = 0U, rb = 0U, sb = 0U, cap = 0U;
    if (!ulebDecode(data, &pos, &addr) || !ulebDecode(data, &pos, &rb) ||
        !ulebDecode(data, &pos, &sb) || !ulebDecode(data, &pos, &cap) || sb == 0U) {
        s->err.set(GS32_ERR_PARAM, "STREAM 配置响应无效");
        return GS32_ERR_PARAM;
    }

    s->stream.configured = true;
    s->stream.periodNs = period_ns;
    s->stream.ringAddr = addr;
    s->stream.ringBytes = rb;
    s->stream.sampleBytes = sb;
    s->stream.capacity = cap;
    s->stream.regionCount = count;
    uint32_t perBlock = kStreamMaxBytes / sb;
    if (perBlock == 0U) perBlock = 1U;
    s->stream.maxSamples = perBlock;
    (void)policy;
    s->stream.readSeq = 0U;
    s->stream.seq = 0U;
    s->err.clear();
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_register_notify(gs32_handle_t h, gs32_notify_cb cb, void* user) {
    auto* s = static_cast<SciState*>(h);
    if (!s || !cb) return GS32_ERR_PARAM;
    s->stream.cb = cb;
    s->stream.user = user;
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_start_stream(gs32_handle_t h) {
    auto* s = static_cast<SciState*>(h);
    if (!s) return GS32_ERR_PARAM;
    if (s->stream.running.load()) return GS32_OK;
    if (s->stream.th.joinable()) s->stream.th.join();
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        if (!s->stream.configured) return GS32_ERR_STATE;
        uint8_t status = 0;
        std::vector<uint8_t> data;
        if (!transactLocked(s, kCmdStreamStart, {}, &status, &data)) return s->err.code();
        s->stream.readSeq = 0U;
        s->stream.seq = 0U;
        s->stream.running.store(true);
    }
    s->stream.th = std::thread(streamThread, s);
    return GS32_OK;
}

GS32LINK_EXPORT int gs32_stop_stream(gs32_handle_t h) {
    auto* s = static_cast<SciState*>(h);
    if (!s) return GS32_ERR_PARAM;
    s->stream.running.store(false);
    if (s->stream.th.joinable()) s->stream.th.join();
    std::lock_guard<std::mutex> lk(s->mtx);
    if (!s->stream.configured) return GS32_OK;
    uint8_t status = 0;
    std::vector<uint8_t> data;
    if (!transactLocked(s, kCmdStreamStop, {}, &status, &data)) return s->err.code();
    return GS32_OK;
}

} /* extern "C" */
