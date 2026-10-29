#ifndef PLOT_WIDGET_H
#define PLOT_WIDGET_H
/*
 * PlotWidget - 单波形窗口
 * 统一主坐标系(所有通道共享 X/Y), 暗色示波器风格。
 * 时间轴使用会话相对时间(秒): 默认跟随最新样本, X 显示 [now-5, now+5];
 * 滚轮/拖动后进入手动视图, 双击恢复跟随与默认 Y 量程。
 */
#include <QWidget>
#include <QHash>
#include <QPointF>
#include <QVector>
#include "qcustomplot.h"

class PlotWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PlotWidget(QWidget* parent = nullptr);

    void addChannel(const QString& name, const QColor& color);
    void removeChannel(const QString& name);
    void setChannelColor(const QString& name, const QColor& color);
    void appendData(const QString& name, double tSec, double value);
    void appendBatch(const QString& name, const QVector<QPointF>& points);
    /* 加入通道时回放已采到的历史(由 MainWindow 提供) */
    void setChannelData(const QString& name, const QVector<QPointF>& points);
    void trimBefore(double tSec);
    void clearData();
    void replot();
    bool hasChannel(const QString& name) const;
    bool hasData() const { return lastT_ != 0.0 || dataPoints_ > 0; }
    double lastTime() const { return lastT_; }
    QStringList channelNames() const { return channels_.keys(); }

    /* 垂直控制 */
    void setChannelDivOffset(const QString& name, double div, double offset, bool autoMode);
    /* 导出当前保留的波形: time_s,channel,value 长表 CSV */
    bool exportCsv(const QString& path, QString* err = nullptr);

private:
    struct Channel {
        QCPGraph* graph = nullptr;
        bool autoMode = true;
        double div = 1.0;
        double offset = 0.0;
    };
    Channel* chan(const QString& name);

    void applyDarkStyle();
    bool eventFilter(QObject* obj, QEvent* ev) override;

    QCustomPlot* plot_ = nullptr;
    QHash<QString, Channel> channels_;
    double windowSec_ = 5.0;     /* 跟随模式下 X 半窗宽(总宽 10s) */
    double historySec_ = 60.0;   /* 数据保留时长 */
    double lastT_ = 0.0;
    qint64 dataPoints_ = 0;
    bool   userZoomed_ = false;  /* X 被手动缩放/拖动 */
    bool   manualY_ = false;     /* Y 量程被手动设定 */
};

#endif /* PLOT_WIDGET_H */
