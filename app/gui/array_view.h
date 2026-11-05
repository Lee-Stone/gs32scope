#ifndef ARRAY_VIEW_H
#define ARRAY_VIEW_H
/*
 * ArrayView - 数组波形组件
 * 手动更新: 右键"更新"时整块读取一次数组并绘制.
 * 组件内不显示坐标系; 放大窗口 x=element, y=value.
 */
#include "core/variable_def.h"
#include <QColor>
#include <QDialog>
#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>
#include <cstdint>
#include <functional>

class QCustomPlot;
class QLabel;

struct ArrayInfo {
    QString  path;
    QString  typeText;          /* 如 "float[1000]" */
    uint32_t addr = 0;
    uint32_t count = 0;
    uint32_t elemSize = 0;
    VariableDef::Type type = VariableDef::Float;
    int bitOffset = -1;
    int bitWidth = 0;

    bool isValid() const {
        return addr != 0 && count > 0 && elemSize > 0 && elemSize <= 8;
    }
};

class ArrayView : public QWidget
{
    Q_OBJECT
public:
    using Reader = std::function<bool(const ArrayInfo&, QVector<double>&, QString*)>;

    explicit ArrayView(QWidget* parent = nullptr);

    void setReader(Reader r) { reader_ = std::move(r); }
    void setLive(bool on);
    void setAccent(const QColor& c);
    void bindArray(const ArrayInfo& a);
    const ArrayInfo& array() const { return array_; }
    bool isLive() const { return live_; }
    /* 右键"更新": 读取一次数组并刷新曲线 */
    void updateOnce();
    void openZoom();
    void setMenuHandler(std::function<void(const QPoint&)> h) {
        menu_ = std::move(h);
    }

protected:
    void contextMenuEvent(QContextMenuEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void sampleNow();
    void applyValues(const QVector<double>& v);
    void setStatus(const QString& text, bool error = false);
    void updateZoom(const QVector<double>& v);

    Reader reader_;
    ArrayInfo array_;
    QVector<double> values_;
    QCustomPlot* mini_ = nullptr;
    QLabel* status_ = nullptr;
    QPointer<QDialog> zoomDlg_;
    QPointer<QCustomPlot> zoomPlot_;
    bool live_ = false;
    QColor accent_ = QColor("#0a6cff");
    std::function<void(const QPoint&)> menu_;
};

#endif /* ARRAY_VIEW_H */
