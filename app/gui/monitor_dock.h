#ifndef MONITOR_DOCK_H
#define MONITOR_DOCK_H
/*
 * MonitorDock - 右侧监控变量栏(树形统计表)
 * 默认列: 波形 | 变量 | 地址 | 当前值; 其余列通过表头右键开启。
 * 波形列勾选 = 加入/移出波形(结构体可整组勾选)。
 */
#include "core/monitor_model.h"
#include <QDockWidget>
#include <QTreeWidget>

class MonitorDock : public QDockWidget
{
    Q_OBJECT
public:
    explicit MonitorDock(QWidget* parent = nullptr);
    void setMonitorModel(MonitorModel* model);

    void rebuild();
    void refresh();

signals:
    void removeRootRequested(int rootIdx);
    void writeRequested(int leafIdx);   /* 双击/右键写入单个叶子 */

private slots:
    void onContextMenu(const QPoint& pos);
    void onHeaderMenu(const QPoint& pos);
    void onItemChanged(QTreeWidgetItem* item, int col);
    void syncDrawChecks();

private:
    enum { ColDraw = 0, ColName = 1, ColType = 2, ColAddr = 3, ColSize = 4,
           ColVal = 5, ColMax = 6, ColMin = 7 };
    void applyShown(QTreeWidgetItem* item, bool on);
    int  leafState(QTreeWidgetItem* item, int& total) const;   /* 返回已显示叶子数 */

    MonitorModel* model_ = nullptr;
    QTreeWidget*  tree_ = nullptr;
    bool rebuilding_ = false;
    bool suppressDrawSync_ = false;
};

#endif /* MONITOR_DOCK_H */
