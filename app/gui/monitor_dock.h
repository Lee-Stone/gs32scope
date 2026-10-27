#ifndef MONITOR_DOCK_H
#define MONITOR_DOCK_H
/*
 * MonitorDock - 右侧监控变量栏(树形统计表)
 * 默认列: 变量 | 地址 | 当前值; 表头右键可勾选显示: 类型/大小/最大/最小/均值。
 */
#include "core/monitor_model.h"
#include <QDockWidget>
#include <QTreeWidget>

class MonitorDock : public QDockWidget
{
    Q_OBJECT
public:
    explicit MonitorDock(QWidget* parent = nullptr);
    void setMonitorModel(MonitorModel* model) { model_ = model; }   /* .ui promoted widget 构造后注入 */

    void rebuild();
    void refresh();

signals:
    void addVariableRequested();
    void removeRootRequested(int rootIdx);

private slots:
    void onContextMenu(const QPoint& pos);
    void onHeaderMenu(const QPoint& pos);

private:
    enum { ColName = 0, ColType = 1, ColAddr = 2, ColSize = 3,
           ColVal = 4, ColMax = 5, ColMin = 6, ColAvg = 7 };
    MonitorModel* model_ = nullptr;
    QTreeWidget*  tree_ = nullptr;
};

#endif /* MONITOR_DOCK_H */
