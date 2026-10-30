#ifndef SAMPLE_ENGINE_H
#define SAMPLE_ENGINE_H
/*
 * SampleEngine - POLL 模式采样引擎
 * 按地址合并连续/重叠的监控区域后整块读取, 叶子从缓冲按偏移裁剪解码。
 * 独立线程 + deadline 调度。
 */
#include "variable_def.h"
#include "monitor_model.h"
#include "link/link_driver.h"
#include <QObject>
#include <QVector>
#include <atomic>
#include <thread>
#include <vector>

struct SampleBatch {
    QVector<qint64> tNs;                 /* 主机时间戳(纳秒) */
    QVector<QVector<double>> values;     /* [叶子][样本], 与 MonitorModel 叶子顺序一致 */
};
Q_DECLARE_METATYPE(SampleBatch)

struct EngineStats {
    double  actualHz = 0.0;
    double  readMs   = 0.0;
    bool    overrun  = false;
    QString error;
    /* 抖动插桩(1s 统计窗口内的峰值, ms): wakeLag = 线程唤醒晚于理想时刻;
     * writeMs = FT_Write 提交耗时(驱动不支持时为 -1) */
    double  wakeLagMs = 0.0;
    double  writeMs   = -1.0;
};
Q_DECLARE_METATYPE(EngineStats)

class SampleEngine : public QObject
{
    Q_OBJECT
public:
    explicit SampleEngine(QObject* parent = nullptr);
    ~SampleEngine() override;

    /* 从监控模型构建聚合读表(引擎不拥有 model, 调用方保证 model 在运行期不变) */
    void configure(LinkDriver* link, int intervalMs, const MonitorModel* model);
    bool start(QString* err);
    /* 单次采样: 读取一拍即自动停止(不发周期调度等待) */
    bool startSingle(QString* err);
    /* STREAM: 驱动按 period 定时采样, 通过 block notify 推送(目标 10kHz) */
    bool startStream(uint32_t periodNs, QString* err);
    bool isStreaming() const { return streamMode_.load(); }
    /* 非阻塞停止: 仅置标志立即返回, worker 退出后发 stopped() 信号
     * (读取中无法中断, stopped 最迟在本次 gs32_read 返回后到达) */
    void requestStop();
    /* 阻塞停止(析构/同步路径用), UI 线程请用 requestStop() */
    void stop();
    bool running() const { return running_.load(); }

signals:
    void batchReady(const SampleBatch& batch);
    void statsUpdated(const EngineStats& stats);
    void stopped();               /* worker 线程已完全退出 */

private:
    void runLoop();
    bool startImpl(bool single, QString* err);
    void onStreamBlock(const gs32_block_hdr_t* hdr, const uint8_t* payload);
    static void streamNotify(gs32_handle_t h, const gs32_block_hdr_t* hdr,
                             const uint8_t* payload, void* user);

    struct LeafSpec {
        int region = -1;
        uint32_t byteOff = 0;
        VariableDef def;
    };

    LinkDriver* link_ = nullptr;
    int intervalMs_ = 20;
    int batchWindowMs_ = 33;            /* POLL 批次送 GUI 的合并窗口, 随采样率调整 */
    std::vector<gs32_mem_region_t> regions_;
    std::vector<LeafSpec> leafSpecs_;
    std::vector<uint8_t>  readBuf_;
    uint32_t regionBytes_ = 0;

    std::atomic<bool> running_{false};
    bool              singleShot_ = false;   /* 单次模式: 一拍即退 */
    std::thread       worker_;

    /* STREAM 状态(notify 回调在驱动线程) */
    std::atomic<bool> streamMode_{false};
    qint64 streamT0Ns_ = 0;
    qint64 streamLastNs_ = 0;
    qint64 streamSamples_ = 0;
    /* 驱动按 10ms 块推送, UI/绘图只需 ~30Hz: 合并小块后再发 batchReady */
    SampleBatch pendingBatch_;
    qint64 pendingEmitNs_ = 0;
};

#endif /* SAMPLE_ENGINE_H */
