#ifndef GS32LINK_H
#define GS32LINK_H
/*
 * GS32Link - GS32Scope 通讯库公共 ABI
 *
 * 纯 C 接口，不依赖 Qt。一个后端编译为一个 DLL（驱动），
 * 导出同一套符号。宿主程序通过 LoadLibrary/QLibrary 加载。
 *
 * 约定:
 *  - 所有函数线程安全性见各函数说明; 句柄级别内部加锁(简单串行化)
 *  - 所有字符串均为 UTF-8
 *  - 布尔返回统一用 int 错误码, 0 = GS32_OK
 */
#include <stdint.h>

#ifdef _WIN32
#  define GS32LINK_EXPORT __declspec(dllexport)
#else
#  define GS32LINK_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 错误码 ---------------- */
enum {
    GS32_OK           =  0,
    GS32_ERR_PARAM    = -1,   /* 参数非法 */
    GS32_ERR_CONNECT  = -2,   /* 连接目标失败 */
    GS32_ERR_READ     = -3,   /* 读内存失败 */
    GS32_ERR_WRITE    = -4,   /* 写内存失败 */
    GS32_ERR_TIMEOUT  = -5,   /* 操作超时 */
    GS32_ERR_NOT_IMPL = -6,   /* 本驱动未实现该能力 */
    GS32_ERR_STATE    = -7,   /* 状态错误(如未连接) */
    GS32_ERR_LOST     = -8    /* 与目标的连接丢失 */
};

/* ---------------- 能力位 ---------------- */
enum {
    GS32_CAP_POLL   = 1u << 0,  /* 支持主机轮询读取 */
    GS32_CAP_STREAM = 1u << 1,  /* 支持硬件定时流式推送(notify) */
    GS32_CAP_WRITE  = 1u << 2   /* 支持采样中写入(预留) */
};

/* ---------------- 基础类型 ---------------- */
typedef void* gs32_handle_t;

/* 连接配置。扩展字段通过 extra(JSON 字符串) 传入, 保持 ABI 稳定 */
typedef struct {
    const char* backend;     /* 后端名, 如 "sim" / "jlink" / "openocd" */
    const char* device;      /* 目标描述: 芯片型号/内核, 如 "Cortex-M4" */
    const char* interface;   /* "swd" / "jtag" */
    uint32_t    speed_khz;   /* 调试时钟频率 */
    const char* extra;       /* JSON 扩展配置, 可为 NULL */
} gs32_config_t;

/* 内存区域 */
typedef struct {
    uint32_t addr;
    uint32_t size;           /* 字节 */
} gs32_mem_region_t;

/* ---------------- 驱动元信息 ----------------
 * 每个驱动 DLL 额外导出 gs32_driver_name, 宿主据此识别后端 */
GS32LINK_EXPORT const char* gs32_driver_name(void);

/* 获取库版本, 返回写入长度, buf 可为 NULL 仅查询长度 */
GS32LINK_EXPORT int gs32_get_version(char* buf, uint32_t buflen);

/* 能力查询(需在 open 之后, h 亦可传 NULL 查询驱动声明能力) */
GS32LINK_EXPORT int gs32_get_caps(gs32_handle_t h, uint32_t* caps);

/* ---------------- 连接管理 ----------------
 * open/close 与 read 可在不同线程调用; read 支持多线程串行化 */
GS32LINK_EXPORT int  gs32_open (const gs32_config_t* cfg, gs32_handle_t* out);
GS32LINK_EXPORT int  gs32_close(gs32_handle_t h);

/* ---------------- 内存读取 ----------------
 * 批量读取 count 个区域, 结果按 regions 顺序紧密拼接到 out_buf。
 * out_buf 需至少 sum(regions[i].size) 字节。timeout_ms<=0 表示驱动默认值。 */
GS32LINK_EXPORT int  gs32_read (gs32_handle_t h,
                                const gs32_mem_region_t* regions, uint32_t count,
                                uint8_t* out_buf, uint32_t buf_size,
                                uint32_t timeout_ms);

/* ---------------- 内存写入(预留, P0 返回 GS32_ERR_NOT_IMPL) ---------------- */
GS32LINK_EXPORT int  gs32_write(gs32_handle_t h,
                                const gs32_mem_region_t* regions, uint32_t count,
                                const uint8_t* in_buf, uint32_t buf_size,
                                uint32_t timeout_ms);

/* 最近一次错误的可读描述(UTF-8), h 为 NULL 时返回全局错误 */
GS32LINK_EXPORT const char* gs32_last_error(gs32_handle_t h);

/* ---------------- 诊断统计(可选实现, 驱动可返回 GS32_ERR_NOT_IMPL) ----------------
 * 采样周期抖动排查: 最近一次批量读取的 FT_Write 提交耗时(ms)。
 * 驱动不支持时 *out_ms 置 -1。 */
GS32LINK_EXPORT int gs32_last_write_ms(gs32_handle_t h, double* out_ms);

/* ---------------- STREAM 模式(P1, 驱动可返回 GS32_ERR_NOT_IMPL) ---------------- */

/* 流式数据块头: 一次 notify 回调 = 一个 Block(header + payload) */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;             /* 'GSNS' = 0x534E5347 */
    uint16_t version;           /* 格式版本, 从 1 开始 */
    uint16_t header_size;       /* 本结构大小(字节) */
    uint64_t seq;               /* Block 连续序号, 丢块检测 */
    uint64_t t0_ns;             /* 本块第一个样本时间戳(硬件时钟域) */
    uint32_t period_ns;         /* 样本间隔; 0 = 不均匀(见 flags bit0) */
    uint16_t sample_count;      /* 本块样本数 N */
    uint16_t region_count;      /* 区域数 R, 与 config 一致 */
    uint32_t flags;             /* bit0 逐样本时间戳 bit1 t0绝对时间
                                   bit2 截断告警    bit3 压缩(预留) */
    uint32_t payload_size;      /* payload 字节数 */
} gs32_block_hdr_t;
#pragma pack(pop)

#define GS32_BLOCK_MAGIC   0x534E5347u
#define GS32_BLOCK_VERSION 1u
#define GS32_BF_PER_SAMPLE_TS (1u << 0)
#define GS32_BF_ABS_TS        (1u << 1)
#define GS32_BF_TRUNCATED     (1u << 2)
#define GS32_BF_COMPRESSED    (1u << 3)

/* notify 回调: 运行在 DLL 内部线程。
 * 契约: 回调内只做拷贝入队, 禁止调用本库阻塞函数 */
typedef void (*gs32_notify_cb)(gs32_handle_t h,
                               const gs32_block_hdr_t* hdr,
                               const uint8_t* payload, void* user);

/* 攒批策略: max_samples 或 max_latency_ns 任一满足即触发 notify */
typedef struct {
    uint32_t max_samples;
    uint32_t max_latency_ns;
} gs32_flush_policy_t;

GS32LINK_EXPORT int gs32_config_stream  (gs32_handle_t h, uint32_t period_ns,
                                         const gs32_mem_region_t* regions, uint32_t count,
                                         const gs32_flush_policy_t* policy);
GS32LINK_EXPORT int gs32_register_notify(gs32_handle_t h, gs32_notify_cb cb, void* user);
GS32LINK_EXPORT int gs32_start_stream   (gs32_handle_t h);
GS32LINK_EXPORT int gs32_stop_stream    (gs32_handle_t h);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GS32LINK_H */
