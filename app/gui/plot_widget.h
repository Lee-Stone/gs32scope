#ifndef PLOT_WIDGET_H
#define PLOT_WIDGET_H
/*
 * PlotWidget - 单波形窗口
 * 叠加显示, 每通道独立右轴, 支持 V/div + offset 手动 / Auto 归一化。
 * 交互: 拖通道垂直=调 offset(内置), 滚轮=调量程, 图例右键「通道设置」精确输入,
 *       双击通道轴=恢复 Auto。
 */
#include <QWidget>
#include <QHash>
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
    void trimBefore(double tSec);
    void replot();
    bool hasChannel(const QString& name) const;
    QStringList channelNames() const { return channels_.keys(); }

    /* 垂直控制 */
    void setChannelDivOffset(const QString& name, double div, double offset, bool autoMode);

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
    double windowSec_ = 30.0;
    bool   userZoomed_ = false;   /* X 被手动缩放 */
    bool   manualY_ = false;      /* Y 量程被手动设定 */
};

#endif /* PLOT_WIDGET_H */
