#include "plot_widget.h"
#include <QMenu>
#include <QMouseEvent>
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
    plot_->xAxis->setRange(-5.0, 5.0);        /* X 默认 [-5,5], 0 居中 */
    plot_->yAxis->setRange(-100.0, 100.0);   /* Y 默认 [-100,100], 0 居中 */
    applyDarkStyle();
    lay->addWidget(plot_, 1);
}

void PlotWidget::applyDarkStyle() {
    const QColor bg(0x14, 0x17, 0x1B), panel(0x1A, 0x1E, 0x24),
        grid(0x2A, 0x2F, 0x36), fg(0xC9, 0xD1, 0xD9);
    plot_->setBackground(bg);
    plot_->axisRect()->setBackground(panel);
    for (auto* ax : {plot_->xAxis, plot_->yAxis, plot_->xAxis2, plot_->yAxis2}) {
        ax->setBasePen(QPen(grid, 1));
        ax->setTickPen(QPen(grid, 1));
        ax->setSubTickPen(QPen(grid.darker(110), 1));
        ax->setTickLabelColor(fg);
        ax->setLabelColor(fg);
        ax->grid()->setPen(QPen(grid, 1, Qt::DotLine));
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
    channels_.insert(name, c);
}

void PlotWidget::removeChannel(const QString& name) {
    auto it = channels_.find(name);
    if (it == channels_.end()) return;
    plot_->removeGraph(it->graph);
    channels_.erase(it);
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
}

void PlotWidget::appendData(const QString& name, double tSec, double value) {
    if (Channel* c = chan(name)) c->graph->addData(tSec, value);
}

void PlotWidget::trimBefore(double tSec) {
    for (auto& c : channels_)
        c.graph->data()->removeBefore(tSec - windowSec_ * 0.2);
}

void PlotWidget::replot() {
    if (!userZoomed_) plot_->xAxis->setRange(-5.0, 5.0);    /* X 默认 [-5,5], 0 居中 */
    if (!manualY_)   plot_->yAxis->setRange(-100.0, 100.0); /* Y 默认 [-100,100], 0 居中 */
    plot_->replot(QCustomPlot::rpQueuedReplot);
}

bool PlotWidget::hasChannel(const QString& name) const {
    return channels_.contains(name);
}

bool PlotWidget::eventFilter(QObject* obj, QEvent* ev) {
    if (obj == plot_ && ev->type() == QEvent::MouseButtonDblClick) {
        userZoomed_ = false;
        manualY_ = false;   /* 双击恢复默认量程 */
        return true;
    }
    if (obj == plot_ && ev->type() == QEvent::Wheel)
        userZoomed_ = true;
    return QWidget::eventFilter(obj, ev);
}
