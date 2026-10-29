#ifndef GS32LINK_DRIVER_H
#define GS32LINK_DRIVER_H
/*
 * LinkDriver - gs32link 驱动 DLL 的宿主侧封装
 * 用 QLibrary 从 drivers/ 目录加载 gs32link_*.dll 并绑定 ABI。
 * 上位机对通讯的全部认知止于 gs32link.h。
 */
#include "gs32link.h"
#include <QCoreApplication>
#include <QDir>
#include <QLibrary>
#include <QString>
#include <QStringList>
#include <functional>

class LinkDriver
{
public:
    LinkDriver() = default;
    ~LinkDriver() { unload(); }

    LinkDriver(const LinkDriver&) = delete;
    LinkDriver& operator=(const LinkDriver&) = delete;

    /* 扫描驱动目录, 返回可用后端名列表(如 ["jlink","sim"]) */
    static QStringList scanDrivers() {
        QStringList names;
        const QString dir = driversDir();
        for (const QFileInfo& fi :
             QDir(dir).entryInfoList({"gs32link_*.dll", "gs32link_*.so"}, QDir::Files)) {
            QLibrary lib(fi.absoluteFilePath());
            if (!lib.load()) continue;
            auto nameFn = reinterpret_cast<const char*(*)()>(lib.resolve("gs32_driver_name"));
            if (nameFn) names << QString::fromUtf8(nameFn());
            lib.unload();
        }
        names.sort();
        return names;
    }

    static QString driversDir() {
        return QCoreApplication::applicationDirPath() + "/drivers";
    }

    /* 加载指定后端驱动 */
    bool load(const QString& backend) {
        unload();
        const QString path = driversDir() + "/gs32link_" + backend + ".dll";
        lib_.setFileName(path);
        if (!lib_.load()) { lastError_ = lib_.errorString(); return false; }
        if (!resolveAll()) { unload(); return false; }
        backend_ = backend;
        return true;
    }

    bool isLoaded() const { return lib_.isLoaded(); }
    const QString& backend() const { return backend_; }
    const QString& lastError() const { return lastError_; }

    /* ---- ABI 透传 ---- */
    bool open(const gs32_config_t& cfg) {
        h_ = nullptr;
        int rc = fn_open_(&cfg, &h_);
        if (rc != GS32_OK) { lastError_ = QString::fromUtf8(fn_last_error_(nullptr)); return false; }
        return true;
    }

    void close() {
        if (h_) { fn_close_(h_); h_ = nullptr; }
    }

    bool read(const gs32_mem_region_t* regions, uint32_t count,
              uint8_t* out, uint32_t size, uint32_t timeout_ms) {
        int rc = fn_read_(h_, regions, count, out, size, timeout_ms);
        if (rc != GS32_OK) lastError_ = QString::fromUtf8(fn_last_error_(h_));
        return rc == GS32_OK;
    }

    bool write(const gs32_mem_region_t* regions, uint32_t count,
               const uint8_t* in, uint32_t size, uint32_t timeout_ms) {
        if (!h_ || !fn_write_) { lastError_ = QStringLiteral("驱动未打开或不支持写入"); return false; }
        int rc = fn_write_(h_, regions, count, in, size, timeout_ms);
        if (rc != GS32_OK) lastError_ = QString::fromUtf8(fn_last_error_(h_));
        return rc == GS32_OK;
    }

    bool caps(uint32_t* c) { return h_ && fn_get_caps_ && fn_get_caps_(h_, c) == GS32_OK; }
    bool hasCap(uint32_t bit) {
        uint32_t c = 0;
        return caps(&c) && (c & bit) != 0;
    }
    QString lastErrorText() const { return lastError_; }

    /* ---- STREAM 透传 ---- */
    bool configStream(uint32_t periodNs, const gs32_mem_region_t* regions, uint32_t count,
                      const gs32_flush_policy_t* policy) {
        if (!h_ || !fn_config_stream_) { lastError_ = QStringLiteral("STREAM 未实现"); return false; }
        const int rc = fn_config_stream_(h_, periodNs, regions, count, policy);
        if (rc != GS32_OK) lastError_ = QString::fromUtf8(fn_last_error_(h_));
        return rc == GS32_OK;
    }
    bool registerNotify(gs32_notify_cb cb, void* user) {
        if (!h_ || !fn_register_notify_) { lastError_ = QStringLiteral("STREAM 未实现"); return false; }
        const int rc = fn_register_notify_(h_, cb, user);
        if (rc != GS32_OK) lastError_ = QString::fromUtf8(fn_last_error_(h_));
        return rc == GS32_OK;
    }
    bool startStream() {
        if (!h_ || !fn_start_stream_) { lastError_ = QStringLiteral("STREAM 未实现"); return false; }
        const int rc = fn_start_stream_(h_);
        if (rc != GS32_OK) lastError_ = QString::fromUtf8(fn_last_error_(h_));
        return rc == GS32_OK;
    }
    bool stopStream() {
        if (!h_ || !fn_stop_stream_) return false;
        const int rc = fn_stop_stream_(h_);
        if (rc != GS32_OK) lastError_ = QString::fromUtf8(fn_last_error_(h_));
        return rc == GS32_OK;
    }

    /* 诊断统计: 最近一次批量读取的 FT_Write 提交耗时(ms)。
     * 旧驱动无此符号时返回 false(不视为错误) */
    bool lastWriteMs(double* out_ms) {
        if (!fn_last_write_ms_) return false;
        return fn_last_write_ms_(h_, out_ms) == GS32_OK;
    }

private:
    template <typename T>
    bool sym(const char* name, T& pfn) {
        pfn = reinterpret_cast<T>(lib_.resolve(name));
        return pfn != nullptr;
    }

    bool resolveAll() {
        if (!sym("gs32_open", fn_open_)) goto fail;
        if (!sym("gs32_close", fn_close_)) goto fail;
        if (!sym("gs32_read", fn_read_)) goto fail;
        if (!sym("gs32_write", fn_write_)) goto fail;
        if (!sym("gs32_last_error", fn_last_error_)) goto fail;
        sym("gs32_last_write_ms", fn_last_write_ms_);   /* 可选: 旧驱动缺失不报错 */
        if (!sym("gs32_get_caps", fn_get_caps_)) goto fail;
        if (!sym("gs32_get_version", fn_get_version_)) goto fail;
        if (!sym("gs32_config_stream", fn_config_stream_)) goto fail;
        if (!sym("gs32_register_notify", fn_register_notify_)) goto fail;
        if (!sym("gs32_start_stream", fn_start_stream_)) goto fail;
        if (!sym("gs32_stop_stream", fn_stop_stream_)) goto fail;
        return true;
    fail:
        lastError_ = lib_.fileName() + ": 缺少 gs32link ABI 符号";
        return false;
    }

    void unload() {
        close();
        if (lib_.isLoaded()) lib_.unload();
        backend_.clear();
    }

    QLibrary lib_;
    QString backend_;
    QString lastError_;
    gs32_handle_t h_ = nullptr;

    int (*fn_open_)(const gs32_config_t*, gs32_handle_t*) = nullptr;
    int (*fn_close_)(gs32_handle_t) = nullptr;
    int (*fn_read_)(gs32_handle_t, const gs32_mem_region_t*, uint32_t,
                    uint8_t*, uint32_t, uint32_t) = nullptr;
    int (*fn_write_)(gs32_handle_t, const gs32_mem_region_t*, uint32_t,
                     const uint8_t*, uint32_t, uint32_t) = nullptr;
    const char* (*fn_last_error_)(gs32_handle_t) = nullptr;
    int (*fn_last_write_ms_)(gs32_handle_t, double*) = nullptr;   /* 可选符号 */
    int (*fn_get_caps_)(gs32_handle_t, uint32_t*) = nullptr;
    int (*fn_get_version_)(char*, uint32_t) = nullptr;
    int (*fn_config_stream_)(gs32_handle_t, uint32_t, const gs32_mem_region_t*,
                             uint32_t, const gs32_flush_policy_t*) = nullptr;
    int (*fn_register_notify_)(gs32_handle_t, gs32_notify_cb, void*) = nullptr;
    int (*fn_start_stream_)(gs32_handle_t) = nullptr;
    int (*fn_stop_stream_)(gs32_handle_t) = nullptr;
};

#endif /* GS32LINK_DRIVER_H */
