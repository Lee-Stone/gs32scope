#ifndef DISPLAY_LIST_H
#define DISPLAY_LIST_H
/*
 * DisplayList - 左下部绘制列表(实时统计表)
 * 列: 颜色 | 变量 | 类型 | 地址 | 大小 | 当前值 | 最大 | 最小 | 平均
 * 变量列吸收剩余宽度铺满底部; 数值随采样实时刷新。
 */
#include "core/monitor_model.h"
#include <QTableWidget>

class DisplayList : public QTableWidget
{
    Q_OBJECT
public:
    explicit DisplayList(QWidget* parent = nullptr);
    void setMonitorModel(MonitorModel* model) { model_ = model; }   /* .ui promoted widget 构造后注入 */

    void rebuild();
    void refreshStats();   /* 采样过程中刷新当前值/最大/最小/平均 */

signals:
    void addVariableRequested();   /* 打开变量对话框添加波形变量 */

private slots:
    void onContextMenu(const QPoint& pos);

private:
    enum { ColColor = 0, ColName = 1, ColType = 2, ColAddr = 3, ColSize = 4,
           ColCur = 5, ColMax = 6, ColMin = 7, ColAvg = 8 };
    MonitorModel* model_ = nullptr;
    bool rebuilding_ = false;
};

#endif /* DISPLAY_LIST_H */
