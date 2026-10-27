#ifndef DISPLAY_LIST_H
#define DISPLAY_LIST_H
/*
 * DisplayList - 左下部绘制列表(表格)
 * 列: 颜色 | 变量 | 类型 | 地址 | 大小 | 最大 | 最小 | Y Div | Y Offset
 * Y Div/Y Offset 可双击编辑, 与波形实时同步。
 */
#include "core/monitor_model.h"
#include <QTableWidget>

class DisplayList : public QTableWidget
{
    Q_OBJECT
public:
    explicit DisplayList(QWidget* parent = nullptr);
    void setMonitorModel(MonitorModel* model) { model_ = model; }   /* .ui promoted widget 构造后注入; 避开 QTableWidget::setModel */

    void rebuild();

signals:
    void channelParamChanged(const QString& path, double div,
                             double offset, bool autoMode);

private slots:
    void onItemChanged(QTableWidgetItem* item);
    void onContextMenu(const QPoint& pos);

private:
    enum { ColColor = 0, ColName = 1, ColType = 2, ColAddr = 3, ColSize = 4,
           ColMax = 5, ColMin = 6, ColDiv = 7, ColOff = 8 };
    MonitorModel* model_ = nullptr;
    bool rebuilding_ = false;
};

#endif /* DISPLAY_LIST_H */
