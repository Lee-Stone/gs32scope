#include "plot_widget.h"
#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QMenu>
#include <QMouseEvent>
#include <QStringConverter>
#include <QTextStream>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>

PlotWidget::PlotWidget(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(0);
    plot_ = new QCustomPlot(this);
    plot_->installEventFilter(this);
    plot_->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectPlottables);
    plot_->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);
    plot_->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
    plot_->setNoAntialiasingOnDrag(true);
    plot_->xAxis->setLabel("t (s)");
    plot_->xAxis->setRange(-5.0, 5.0);        /* 无数据时 X 默认 [-5,5], 0 居中 */
    plot_->yAxis->setRange(-100.0, 100.0);   /* Y 默认 [-100,100], 0 居中 */
    applyStyle();
    lay->addWidget(plot_, 1);
}

void PlotWidget::setDarkTheme(bool dark) {
    if (dark_ == dark) return;
    dark_ = dark;
    applyStyle();
    replot();
}

void PlotWidget::applyStyle() {
    const QColor bg    = dark_ ? QColor(0x14, 0x17, 0x1B) : QColor(0xFF, 0xFF, 0xFF);
    const QColor panel = dark_ ? QColor(0x1A, 0x1E, 0x24) : QColor(0xF8, 0xFA, 0xFD);
    const QColor axis  = dark_ ? QColor(0x2A, 0x2F, 0x36) : QColor(0xB8, 0xC0, 0xCC);
    const QColor grid  = dark_ ? QColor("#55657a") : QColor("#D4DAE3");
    const QColor fg    = dark_ ? QColor(0xC9, 0xD1, 0xD9) : QColor(0x38, 0x41, 0x4D);
    const QColor zero  = dark_ ? QColor("#93a4b8") : QColor("#8A94A6");
    plot_->setBackground(bg);
    plot_->axisRect()->setBackground(panel);
    for (auto* ax : {plot_->xAxis, plot_->yAxis, plot_->xAxis2, plot_->yAxis2}) {
        ax->setBasePen(QPen(axis, 1));
        ax->setTickPen(QPen(axis, 1));
        ax->setSubTickPen(QPen(axis.lighter(dark_ ? 110 : 115), 1));
        ax->setTickLabelColor(fg);
        ax->setLabelColor(fg);
        ax->grid()->setVisible(true);
        ax->grid()->setPen(QPen(grid, 1, Qt::SolidLine));   /* 高对比主网格 */
        ax->grid()->setSubGridVisible(false);
        ax->grid()->setZeroLinePen(QPen(zero, 1, Qt::DashLine));
    }
    plot_->axisRect()->setAutoMargins(QCP::msLeft | QCP::msBottom | QCP::msTop);
    plot_->axisRect()->setMargins(QMargins(0, 0, 0, 0));   /* 无右侧多轴空间 */
    plot_->yAxis->setVisible(true);         /* 显示左侧 Y 坐标 */
    plot_->legend->setVisible(false);       /* 无曲线标识 */
}

PlotWidget::Channel* PlotWidget::chan(const QString& name) {
    auto it = channels_.find(name);
    return it == channels_.end() ? nullptr : &it.value();
}

void PlotWidget::addChannel(const QString& name, const QColor& color) {
    if (channels_.contains(name)) return;
    Channel c;
    c.graph = plot_->addGraph(plot_->xAxis, plot_->yAxis);  /* 统一主坐标系 */
    c.graph->setPen(QPen(color, 1.2));
    c.graph->setSelectable(QCP::stDataRange);
    c.graph->setAdaptiveSampling(true);   /* 大数据量时自动抽稀绘制 */
    c.graph->setLineStyle(QCPGraph::lsLine);
    channels_.insert(name, c);
}

void PlotWidget::removeChannel(const QString& name) {
    auto it = channels_.find(name);
    if (it == channels_.end()) return;
    plot_->removeGraph(it->graph);
    channels_.erase(it);
    replot();
}

void PlotWidget::setChannelColor(const QString& name, const QColor& color) {
    if (Channel* c = chan(name))
        c->graph->setPen(QPen(color, 1.2));
}

void PlotWidget::setChannelDivOffset(const QString& name, double div,
                                     double offset, bool autoMode) {
    if (Channel* c = chan(name)) {
        c->div = div > 0 ? div : 1.0;
        c->offset = offset;
        c->autoMode = autoMode;
    }
    /* 主 Y 轴量程(所有通道共享): 5 格 × V/div, 以 offset 为中心 */
    constexpr int kDivs = 5;
    if (autoMode) {
        manualY_ = false;
    } else {
        manualY_ = true;
        const double d = div > 0 ? div : 1.0;
        plot_->yAxis->setRange(offset - d * kDivs / 2, offset + d * kDivs / 2);
    }
    replot();
}

void PlotWidget::appendData(const QString& name, double tSec, double value) {
    if (Channel* c = chan(name)) {
        c->graph->addData(tSec, value);
        if (tSec > lastT_) lastT_ = tSec;
        ++dataPoints_;
    }
}

void PlotWidget::appendBatch(const QString& name, const QVector<QPointF>& points) {
    if (Channel* c = chan(name)) {
        /* 整批追加: 逐点 addData 会在 10kHz 下反复排序/插入, GUI 事件队列会积压 */
        QVector<double> keys, vals;
        keys.reserve(points.size());
        vals.reserve(points.size());
        for (const auto& p : points) {
            keys.append(p.x());
            vals.append(p.y());
            if (p.x() > lastT_) lastT_ = p.x();
        }
        if (!keys.isEmpty()) c->graph->addData(keys, vals, true);
        dataPoints_ += points.size();
        /* 高采样率下限制绘图点数: 10kHz 保留约 12s, 低采样率仍保留 60s */
        constexpr int kMaxPlotPoints = 120000;
        if (c->graph->dataCount() > kMaxPlotPoints)
            c->graph->data()->removeBefore(lastT_ - 12.0);
    }
}

void PlotWidget::setChannelData(const QString& name, const QVector<QPointF>& points) {
    Channel* c = chan(name);
    if (!c) return;
    QVector<double> keys, vals;
    keys.reserve(points.size());
    vals.reserve(points.size());
    for (const auto& p : points) {
        keys.append(p.x());
        vals.append(p.y());
        if (p.x() > lastT_) lastT_ = p.x();
    }
    c->graph->setData(keys, vals, true);
    dataPoints_ += points.size();
}

void PlotWidget::trimBefore(double tSec) {
    const double cutoff = tSec - historySec_;
    for (auto& c : channels_)
        c.graph->data()->removeBefore(cutoff);
}

void PlotWidget::clearData() {
    for (auto& c : channels_)
        c.graph->data()->clear();
    lastT_ = 0.0;
    displayT_ = 0.0;
    displayInit_ = false;
    dataPoints_ = 0;
    replot();
}

namespace {
/* 量程极小时固定精度会让所有刻度四舍五入成同一个数字; 随量程加密小数位 */
void setAxisPrecision(QCPAxis* ax, double range) {
    if (!ax) return;
    const double r = std::max(range, 1e-12);
    const int digits = std::clamp(int(std::ceil(-std::log10(r))) + 2, 0, 9);
    ax->setNumberFormat(QStringLiteral("f"));
    ax->setNumberPrecision(digits);
}
} // namespace

void PlotWidget::updateYRange() {
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    const QCPRange keyRange = plot_->xAxis->range();
    for (auto it = channels_.constBegin(); it != channels_.constEnd(); ++it) {
        bool found = false;
        const QCPRange r = it.value().graph->getValueRange(found, QCP::sdBoth, keyRange);
        if (!found || !std::isfinite(r.lower) || !std::isfinite(r.upper)) continue;
        lo = std::min(lo, r.lower);
        hi = std::max(hi, r.upper);
    }
    if (!std::isfinite(lo) || !std::isfinite(hi) || hi < lo) {
        plot_->yAxis->setRange(-100.0, 100.0);
        setAxisPrecision(plot_->yAxis, 200.0);
        return;
    }

    /* 可见数据上下留 8% 余量; 常量信号给一个可读的小量程 */
    const double span = hi - lo;
    const double pad = span > 1e-12 ? span * 0.08
                                    : std::max(0.01, std::abs(lo) * 0.05);
    const double lower = lo - pad;
    const double upper = hi + pad;
    plot_->yAxis->setRange(lower, upper);

    /* 刻度精度随量程变化: 2 的量程 -> 0.01 级显示 */
    setAxisPrecision(plot_->yAxis, upper - lower);
}

void PlotWidget::replot() {
    if (!userZoomed_) {
        /* 跟随最新样本: 显示位置向 lastT_ 平滑逼近, 避免 30fps 下按批跳动 */
        if (!displayInit_ || std::abs(lastT_ - displayT_) > 2.0) {
            displayT_ = lastT_;
            displayInit_ = true;
        } else {
            displayT_ += (lastT_ - displayT_) * 0.35;
            if (std::abs(lastT_ - displayT_) < 0.005) displayT_ = lastT_;
        }
        plot_->xAxis->setRange(displayT_ - windowSec_, displayT_ + windowSec_);
    }
    if (!manualY_) updateYRange();
    else setAxisPrecision(plot_->yAxis, plot_->yAxis->range().size());
    setAxisPrecision(plot_->xAxis, plot_->xAxis->range().size());
    plot_->replot(QCustomPlot::rpQueuedReplot);
}

bool PlotWidget::hasChannel(const QString& name) const {
    return channels_.contains(name);
}

bool PlotWidget::eventFilter(QObject* obj, QEvent* ev) {
    if (obj == plot_) {
        if (ev->type() == QEvent::MouseButtonDblClick) {
            userZoomed_ = false;
            manualY_ = false;   /* 双击恢复跟随 + 默认量程 */
            displayInit_ = false;
            replot();
            return true;
        }
        if (ev->type() == QEvent::Wheel) {
            userZoomed_ = true;
            manualY_ = true;    /* 保留用户缩放后的 Y 量程/刻度 */
        }
        if (ev->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton) {
                userZoomed_ = true;
                manualY_ = true;   /* 拖动/点击后不把 Y 拉回 -100..100 */
            }
        }
    }
    return QWidget::eventFilter(obj, ev);
}

bool PlotWidget::exportCsv(const QString& path, QString* err) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = QStringLiteral("无法写入: %1").arg(path);
        return false;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << "\xEF\xBB\xBF";   /* UTF-8 BOM, Excel 中文不乱码 */
    ts << "time_s,channel,value\n";
    static const int kPrecision = 9;
    for (auto it = channels_.constBegin(); it != channels_.constEnd(); ++it) {
        const QString name = it.key();
        auto data = it.value().graph->data();
        for (auto d = data->constBegin(); d != data->constEnd(); ++d) {
            ts << QString::number(d->key, 'f', kPrecision) << ','
               << name << ','
               << QString::number(d->value, 'f', kPrecision) << '\n';
        }
    }
    f.close();
    return true;
}
