#ifndef DASHBOARD_WIDGET_H
#define DASHBOARD_WIDGET_H
/*
 * DashboardWidget - 自定义图形化仪表盘
 * 组件: 数值(显示+写入) / 圆形仪表 / 滑块 / 开关, 每个组件绑定一个监控变量。
 * 布局可拖动, 退出时通过 QSettings 保存, 启动恢复。
 */
#include "array_view.h"
#include <QColor>
#include <QPoint>
#include <QVector>
#include <QWidget>
#include <functional>
#include <utility>

class QLabel;
class QMenu;
class QScrollArea;
class QSettings;
class MonitorModel;
class DashboardItem;

class DashboardWidget : public QWidget
{
    Q_OBJECT
public:
    enum class Kind { Value, Gauge, Slider, Toggle, Editor, Array };

    explicit DashboardWidget(QWidget* parent = nullptr);
    ~DashboardWidget() override;

    void setMonitorModel(MonitorModel* model) { model_ = model; }
    /* 数组组件: 符号目录 + 读取回调 + 连接状态 */
    using ArrayReader = std::function<bool(const ArrayInfo&, QVector<double>&, QString*)>;
    void setArrayCatalog(const QVector<ArrayInfo>& v) { arrays_ = v; }
    const QVector<ArrayInfo>& arrayCatalog() const { return arrays_; }
    void setArrayReader(ArrayReader r);
    void setArrayLive(bool on);
    bool arrayLive() const { return arrayLive_; }
    bool sampleArray(const ArrayInfo& a, QVector<double>& out, QString* err);
    void refresh();
    void saveState(QSettings& s) const;
    void restoreState(QSettings& s);
    void createDemoPanel();
    void clearAll();
    /* 数据菜单: 添加组件的子菜单(数值/仪表/滑块/开关/数组) */
    void populateAddMenu(QMenu* menu);
    int  itemCount() const { return items_.size(); }

signals:
    void writeRequested(const QString& path, double value);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;

private:
    friend class DashboardItem;
    friend class DashboardCanvas;
    void addItem(Kind kind, const QString& path, const QString& label,
                 double minV, double maxV, const QPoint& pos = QPoint(-1, -1));
    void addItemInteractive(Kind kind);
    void addArrayItem(const ArrayInfo& a, const QString& label);
    void bindItem(DashboardItem* item);
    void removeItem(DashboardItem* item);
    /* 统一 240x180 单元网格: 自动吸附、禁止重叠 */
    QPoint firstFreeCell() const;
    QPoint cellPos(int col, int row) const;
    int  colCount() const;
    int  rowCount() const;
    int  tileWidth() const;
    bool cellOccupied(int col, int row, DashboardItem* self) const;
    void layoutItemsToCells();
    void updateEmptyHint();
    QColor accentFor(int index) const;
    QPoint snapPosition(const QPoint& proposed, const QSize& size, DashboardItem* self);
    void clearSnapGuides();

    MonitorModel* model_ = nullptr;
    QVector<ArrayInfo> arrays_;
    ArrayReader arrayReader_;
    bool arrayLive_ = false;
    QWidget* canvas_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QLabel*  emptyHint_ = nullptr;
    QVector<DashboardItem*> items_;
    int grid_ = 20;        /* 背景细网格 */
    int tileW_ = 240;      /* 统一组件宽 */
    int tileH_ = 180;      /* 统一组件高 */
    int gap_ = 20;
    int margin_ = 20;
    int lastCols_ = 0;
    int snapX_ = -1;
    int snapY_ = -1;
};

#endif /* DASHBOARD_WIDGET_H */
