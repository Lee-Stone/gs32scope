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

PlotWidget::PlotWidget(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(0);
    plot_ = new QCustomPlot(this);
    plot_->installEventFilter(this);
    plot_->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom | QCP::iSelectPlottables);
    plot_->setNoAntialiasingOnDrag(true);
    plot_->xAxis->setLabel("t (s)");
    plot_->xAxis->setRange(-5.0, 5.0);        /* 无数据时 X 默认 [-5,5], 0 居中 */
    plot_->yAxis->setRange(-100.0, 100.0);   /* Y 默认 [-100,100], 0 居中 */
    applyDarkStyle();
    lay->addWidget(plot_, 1);
}

void PlotWidget::applyDarkStyle() {
    const QColor bg(0x14, 0x17, 0x1B), panel(0x1A, 0x1E, 0x24),
        grid(0x2A, 0x2F, 0x36), fg(0xC9, 0xD1, 0xD9);
    plot_->setBackground(bg);
    plot_->axisRect()->setBackground(panel);
    const QColor gridLine("#55657a");
    for (auto* ax : {plot_->xAxis, plot_->yAxis, plot_->xAxis2, plot_->yAxis2}) {
        ax->setBasePen(QPen(grid, 1));
        ax->setTickPen(QPen(grid, 1));
        ax->setSubTickPen(QPen(grid.darker(110), 1));
        ax->setTickLabelColor(fg);
        ax->setLabelColor(fg);
        ax->grid()->setVisible(true);
        ax->grid()->setPen(QPen(gridLine, 1, Qt::SolidLine));   /* 高对比主网格 */
        ax->grid()->setSubGridVisible(false);
        ax->grid()->setZeroLinePen(QPen(QColor("#93a4b8"), 1, Qt::DashLine));
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
        for (const auto& p : points) {
            c->graph->addData(p.x(), p.y());
            if (p.x() > lastT_) lastT_ = p.x();
        }
        dataPoints_ += points.size();
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
    dataPoints_ = 0;
    replot();
}

void PlotWidget::replot() {
    if (!userZoomed_) {
        /* 跟随最新样本: now 居中, 左右各 5s(无数据时即 [-5,5]) */
        plot_->xAxis->setRange(lastT_ - windowSec_, lastT_ + windowSec_);
    }
    if (!manualY_) plot_->yAxis->setRange(-100.0, 100.0);
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
            replot();
            return true;
        }
        if (ev->type() == QEvent::Wheel) {
            userZoomed_ = true;
        }
        if (ev->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton) userZoomed_ = true;
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
