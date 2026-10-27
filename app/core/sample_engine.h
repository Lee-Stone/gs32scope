#ifndef SAMPLE_ENGINE_H
#define SAMPLE_ENGINE_H
/*
 * SampleEngine - POLL 模式采样引擎
 * 按监控根(结构体整块/单变量)聚合读取: 每个根一个内存区域, 一次事务读整块,
 * 叶子从缓冲按偏移裁剪解码。独立线程 + deadline 调度。
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

    struct LeafSpec {
        int region = -1;
        uint32_t byteOff = 0;
        VariableDef def;
    };

    LinkDriver* link_ = nullptr;
    int intervalMs_ = 20;
    std::vector<gs32_mem_region_t> regions_;
    std::vector<LeafSpec> leafSpecs_;
    std::vector<uint8_t>  readBuf_;

    std::atomic<bool> running_{false};
    bool              singleShot_ = false;   /* 单次模式: 一拍即退 */
    std::thread       worker_;
};

#endif /* SAMPLE_ENGINE_H */
