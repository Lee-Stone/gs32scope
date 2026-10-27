#ifndef MONITOR_MODEL_H
#define MONITOR_MODEL_H
/*
 * MonitorModel - 监控集数据模型
 * 双结构: 节点树(MonitorNode, 保留文件层级用于显示) + 叶子扁平表(供引擎/统计)。
 * 统计: 当前值/min/max/均值(增量 O(1)); 颜色: 每根分配波形通道色(仅波形用)。
 */
#include "core/variable_def.h"
#include "symbols/dwarf_parser.h"
#include <QColor>
#include <QHash>
#include <QObject>
#include <QVector>

struct MonitorNode {
    QString name;            /* 显示段 */
    QString path;            /* 全路径 */
    VariableDef def;
    bool isStruct = false;
    int  leafIdx = -1;       /* 叶子在扁平表的下标; 结构体节点为 -1 */
    QVector<MonitorNode*> children;
    ~MonitorNode() { qDeleteAll(children); }
};

struct MonitorLeaf {
    QString     path;
    VariableDef def;
    int         rootIdx = -1;
    double      cur = 0, min = 0, max = 0, sum = 0;
    qint64      n = 0;
    bool        shown = false;     /* 绘制列表/波形 */
    bool        monitored = true;  /* 监控列表(右侧); 绘制专用叶子为 false */
};

struct MonitorRoot {
    QString name;
    QString typeText;
    uint32_t addr = 0, size = 0;
    bool isStruct = false;
    MonitorNode* tree = nullptr;   /* 显示树根(叶根=单节点) */
    QVector<int> leaves;           /* 扁平叶子下标(采样/统计) */
    QColor color;
    ~MonitorRoot() { delete tree; }
};

class MonitorModel : public QObject
{
    Q_OBJECT
public:
    explicit MonitorModel(QObject* parent = nullptr);
    ~MonitorModel() override;

    void addLeafRoot(const VariableDef& v);
    void addDwarfRoot(const DwarfVar& v);
    int  addDrawOnlyRoot(const VariableDef& v);  /* 仅绘制(不进监控列表), 返回叶子下标 */
    void removeRoot(int rootIdx);
    void clear();
    void setLeafMonitored(int leafIdx, bool m);  /* 升级为监控(或移除监控标记) */

    int rootCount() const { return roots_.size(); }
    int leafCount() const { return leaves_.size(); }
    const MonitorRoot& root(int i) const { return *roots_.at(i); }
    const MonitorLeaf& leaf(int i) const { return leaves_.at(i); }
    int leafIndexByPath(const QString& path) const { return leafIdx_.value(path, -1); }

    void updateLeafValue(int leafIdx, double v);
    void setLeafShown(int leafIdx, bool on);
    void resetStats();

signals:
    void changed();
    void leafValueChanged(int leafIdx);
    void leafShownChanged(int leafIdx);

private:
    MonitorNode* buildNode(const DwarfVar& v, const QString& prefix,
                           QVector<int>& flatLeaves, int rootIdx);
    int  addFlatLeaf(const QString& path, const VariableDef& d, int rootIdx);
    QColor nextColor();

    QVector<MonitorRoot*> roots_;   /* 指针向量: 避免含裸指针的浅拷贝导致悬垂 */
    QVector<MonitorLeaf> leaves_;
    QHash<QString, int>  leafIdx_;
    int colorIdx_ = 0;
};

#endif /* MONITOR_MODEL_H */
