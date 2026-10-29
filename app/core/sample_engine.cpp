#include "sample_engine.h"
#include "value_codec.h"
#include <chrono>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>   /* timeBeginPeriod/timeEndPeriod (WIN32_LEAN_AND_MEAN 下不随 windows.h 引入) */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace {
using Clock = std::chrono::steady_clock;
inline qint64 nsSinceEpoch(const Clock::time_point& tp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               tp.time_since_epoch()).count();
}
} // namespace

SampleEngine::SampleEngine(QObject* parent) : QObject(parent) {}
SampleEngine::~SampleEngine() { stop(); }

void SampleEngine::configure(LinkDriver* link, int intervalMs,
                             const MonitorModel* model) {
    link_ = link;
    intervalMs_ = std::max(1, intervalMs);

    regions_.clear();
    leafSpecs_.clear();
    /* readBuf_ 是全部 region 顺序拼接的总缓冲:
     * 叶子偏移 = 前面 region 累计长度 + region 内偏移(变量地址 - 根地址)。
     * 早期版本漏加累计偏移, 导致第二个及以后的变量都读到第一块的数据 */
    uint32_t regionOff = 0;
    for (int ri = 0; ri < model->rootCount(); ++ri) {
        const auto& r = model->root(ri);
        regions_.push_back({r.addr, r.size});
        for (int li : r.leaves) {
            const auto& lf = model->leaf(li);
            LeafSpec s;
            s.region = ri;
            s.byteOff = regionOff + (lf.def.addr - r.addr);
            s.def = lf.def;
            leafSpecs_.push_back(s);
        }
        regionOff += r.size;
    }
    uint32_t total = 0;
    for (const auto& r : regions_) total += r.size;
    regionBytes_ = total;
    readBuf_.resize(total ? total : 4);
}

bool SampleEngine::start(QString* err) { return startImpl(false, err); }
bool SampleEngine::startSingle(QString* err) { return startImpl(true, err); }

bool SampleEngine::startStream(uint32_t periodNs, QString* err) {
    if (running_.load()) return true;
    if (worker_.joinable()) worker_.join();
    if (!link_ || !link_->isLoaded() || leafSpecs_.empty() || regions_.empty()) {
        if (err) *err = QStringLiteral("引擎未配置或无监控变量");
        return false;
    }
    if (!link_->hasCap(GS32_CAP_STREAM)) {
        if (err) *err = QStringLiteral("当前后端不支持 STREAM");
        return false;
    }
    gs32_flush_policy_t policy{100, 10000000};   /* 100 点或 10ms 推一块 */
    if (!link_->configStream(periodNs, regions_.data(), uint32_t(regions_.size()), &policy)) {
        if (err) *err = QStringLiteral("STREAM 配置失败: ") + link_->lastErrorText();
        return false;
    }
    if (!link_->registerNotify(&SampleEngine::streamNotify, this)) {
        if (err) *err = QStringLiteral("STREAM 注册回调失败: ") + link_->lastErrorText();
        return false;
    }
    streamT0Ns_ = streamLastNs_ = 0;
    streamSamples_ = 0;
    pendingBatch_ = SampleBatch();
    pendingEmitNs_ = 0;
    streamMode_.store(true);
    running_.store(true);
    if (!link_->startStream()) {
        streamMode_.store(false);
        running_.store(false);
        if (err) *err = QStringLiteral("STREAM 启动失败: ") + link_->lastErrorText();
        return false;
    }
    return true;
}

void SampleEngine::streamNotify(gs32_handle_t, const gs32_block_hdr_t* hdr,
                                const uint8_t* payload, void* user) {
    static_cast<SampleEngine*>(user)->onStreamBlock(hdr, payload);
}

void SampleEngine::onStreamBlock(const gs32_block_hdr_t* hdr, const uint8_t* payload) {
    if (!hdr || !payload || !hdr->sample_count || !regionBytes_) return;
    if (hdr->region_count != regions_.size()) return;
    const uint32_t n = hdr->sample_count;
    if (uint64_t(hdr->payload_size) < uint64_t(n) * regionBytes_) return;

    const qint64 period = hdr->period_ns ? qint64(hdr->period_ns) : 1;
    SampleBatch batch;
    batch.tNs.resize(int(n));
    batch.values.resize(leafSpecs_.size());
    for (uint32_t i = 0; i < n; ++i)
        batch.tNs[int(i)] = qint64(hdr->t0_ns) + qint64(i) * period;
    for (auto& v : batch.values) v.resize(int(n));

    for (uint32_t s = 0; s < n; ++s) {
        const uint8_t* sample = payload + size_t(s) * regionBytes_;
        for (size_t k = 0; k < leafSpecs_.size(); ++k) {
            const LeafSpec& spec = leafSpecs_[k];
            const uint8_t* p = sample + spec.byteOff;
            double v = gs32::decodeValue(spec.def, p);
            if (spec.def.bitOffset >= 0)
                v = gs32::decodeBitfield(spec.def, gs32::readRaw(spec.def, p));
            batch.values[int(k)][int(s)] = v;
        }
    }

    if (streamT0Ns_ == 0) streamT0Ns_ = batch.tNs.first();
    streamLastNs_ = batch.tNs.last();
    streamSamples_ += n;
    if (streamLastNs_ > streamT0Ns_ &&
        streamLastNs_ - streamT0Ns_ >= 1000000000LL) {
        EngineStats st;
        st.actualHz = double(streamSamples_) * 1e9 /
                      double(streamLastNs_ - streamT0Ns_);
        st.readMs = 0.0;
        st.overrun = false;
        st.wakeLagMs = 0.0;
        st.writeMs = -1.0;
        emit statsUpdated(st);
        streamT0Ns_ = streamLastNs_;
        streamSamples_ = 0;
    }
    if (pendingBatch_.tNs.isEmpty()) {
        pendingBatch_ = batch;
    } else {
        pendingBatch_.tNs += batch.tNs;
        if (pendingBatch_.values.size() == batch.values.size()) {
            for (int k = 0; k < batch.values.size(); ++k)
                pendingBatch_.values[k] += batch.values[k];
        } else {
            pendingBatch_ = batch;
        }
    }
    /* 驱动按 10ms 块推送; GUI 以 ~30Hz 消费合并批, 避免 100 batch/s 压垮事件队列 */
    const qint64 lastT = batch.tNs.last();
    if (pendingEmitNs_ == 0 || lastT - pendingEmitNs_ >= 33000000LL ||
        pendingBatch_.tNs.size() >= 2000) {
        emit batchReady(pendingBatch_);
        pendingBatch_ = SampleBatch();
        pendingEmitNs_ = lastT;
    }
}

bool SampleEngine::startImpl(bool single, QString* err) {
    if (running_.load()) return true;
    singleShot_ = single;
    streamMode_.store(false);
    /* 回收上一次已结束的 worker(requestStop 后 runLoop 已退出, join 立即返回),
     * 否则对 joinable 的 std::thread 赋值会触发 terminate */
    if (worker_.joinable()) worker_.join();
    if (!link_ || !link_->isLoaded() || leafSpecs_.empty()) {
        if (err) *err = QStringLiteral("引擎未配置或无监控变量");
        return false;
    }
#ifdef _WIN32
    /* Windows 默认睡眠粒度 15.6ms, 提到 1ms 才能做 ms 级采样调度
     * (stop 时 timeEndPeriod 恢复; 引用计数, 嵌套安全) */
    timeBeginPeriod(1);
#endif
    running_.store(true);
    worker_ = std::thread([this] { runLoop(); });
    return true;
}

void SampleEngine::requestStop() {
    if (streamMode_.exchange(false)) {
        if (link_) link_->stopStream();
        running_.store(false);
        emit stopped();
        return;
    }
    running_.store(false);
}

void SampleEngine::stop() {
    if (streamMode_.exchange(false)) {
        if (link_) link_->stopStream();
        running_.store(false);
        return;
    }
    running_.store(false);
    if (worker_.joinable()) worker_.join();
}

void SampleEngine::runLoop() {
    using namespace std::chrono;
    const auto interval = milliseconds(intervalMs_);
    auto tNext = Clock::now();

#ifdef _WIN32
    /* 高精度 waitable timer(Win10 1803+): 亚毫秒睡眠粒度,
     * 不依赖 timeBeginPeriod, 也不受 Win11 后台进程策略影响;
     * 创建失败(旧系统)则退回 sleep_until + timeBeginPeriod 路径 */
    HANDLE hTimer = CreateWaitableTimerExW(nullptr, nullptr,
                                           CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
#endif

    qint64 statT0Ns = nsSinceEpoch(tNext);
    int    statCnt  = 0;
    double readMsAcc = 0.0;
    bool   overrun = false;
    /* 抖动插桩: 1s 窗口内 wakeLag(唤醒延迟)/writeMs(FT_Write 提交) 峰值 */
    double wakeLagMaxMs = 0.0;
    double writeMaxMs = -1.0;
    /* 读取错误节流: 同文本最多 1 次/秒, 防止错误风暴塞爆 UI 事件循环 */
    QString lastErr;
    qint64  lastErrNs = 0;

    SampleBatch batch;
    batch.values.resize(leafSpecs_.size());

    while (running_.load()) {
        /* 插桩: 本拍唤醒时刻相对理想时刻的滞后(负=提前唤醒, 取正峰值) */
        const double wakeLagMs = duration<double, std::milli>(
                                     Clock::now() - tNext).count();
        if (wakeLagMs > wakeLagMaxMs) wakeLagMaxMs = wakeLagMs;

        const auto tRead = Clock::now();
        const qint64 tReadNs = nsSinceEpoch(tRead);

        const auto t0 = Clock::now();
        const bool ok = link_->read(regions_.data(), (uint32_t)regions_.size(),
                                    readBuf_.data(), (uint32_t)readBuf_.size(), 0);
        const double ms = duration<double, std::milli>(Clock::now() - t0).count();
        readMsAcc = 0.9 * readMsAcc + 0.1 * ms;
        overrun = (ms > 0.5 * intervalMs_);
        {   /* 插桩: 驱动侧 FT_Write 提交耗时(旧驱动无符号则保持 -1) */
            double wms = -1.0;
            if (link_->lastWriteMs(&wms) && wms > writeMaxMs) writeMaxMs = wms;
        }

        if (ok) {
            for (size_t i = 0; i < leafSpecs_.size(); ++i) {
                const LeafSpec& s = leafSpecs_[i];
                const uint8_t* p = readBuf_.data() + s.byteOff;
                double v = gs32::decodeValue(s.def, p);
                if (s.def.bitOffset >= 0)
                    v = gs32::decodeBitfield(s.def, gs32::readRaw(s.def, p));
                batch.values[i].append(v);
            }
            batch.tNs.append(tReadNs);
            ++statCnt;

            if (tReadNs - batch.tNs.first() > 200'000'000 ||
                batch.tNs.size() >= 256) {
                emit batchReady(batch);
                batch.tNs.clear();
                for (auto& v : batch.values) v.clear();
            }
        } else {
            const QString e = link_->lastErrorText();
            const qint64 nowNs = nsSinceEpoch(Clock::now());
            if (e != lastErr || nowNs - lastErrNs >= 1'000'000'000) {
                EngineStats st;
                st.error = e;
                emit statsUpdated(st);
                lastErr = e;
                lastErrNs = nowNs;
            }
        }

        /* 单次采样: 一拍即退(跳过周期调度等待), 循环外统一 flush+stopped */
        if (singleShot_) { running_.store(false); break; }

        tNext += interval;
        if (tNext < Clock::now()) { tNext = Clock::now(); overrun = true; }
#ifdef _WIN32
        if (hTimer) {
            const qint64 remainNs = duration_cast<nanoseconds>(
                                        tNext - Clock::now()).count();
            if (remainNs > 0) {
                /* 负 due = 相对时刻; 100ns 单位向上取整避免提前唤醒 */
                LARGE_INTEGER due;
                due.QuadPart = -((remainNs + 99) / 100);
                if (SetWaitableTimer(hTimer, &due, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(hTimer, INFINITE);
            }
        } else {
            std::this_thread::sleep_until(tNext);
        }
#else
        std::this_thread::sleep_until(tNext);
#endif

        const qint64 nowNs = nsSinceEpoch(Clock::now());
        if (nowNs - statT0Ns >= 1'000'000'000) {
            EngineStats st;
            st.actualHz = statCnt * 1e9 / double(nowNs - statT0Ns);
            st.readMs = readMsAcc;
            st.overrun = overrun;
            st.wakeLagMs = wakeLagMaxMs;
            st.writeMs = writeMaxMs;
            emit statsUpdated(st);
            statT0Ns = nowNs;
            statCnt = 0;
            wakeLagMaxMs = 0.0;
            writeMaxMs = -1.0;
        }
    }
    if (!batch.tNs.isEmpty()) emit batchReady(batch);
#ifdef _WIN32
    if (hTimer) CloseHandle(hTimer);
    timeEndPeriod(1);   /* 恢复系统定时器分辨率 */
#endif
    emit stopped();   /* worker 即将完全退出, UI 可安全 reconfigure/断开 */
}
