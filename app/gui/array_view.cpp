#include "array_view.h"

#include <QContextMenuEvent>
#include <QDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include "qcustomplot.h"

namespace {
QVector<double> indexKeys(int n) {
    QVector<double> k;
    k.resize(n);
    for (int i = 0; i < n; ++i) k[i] = i;
    return k;
}

void styleMini(QCustomPlot* p, const QColor& accent) {
    p->addGraph();
    p->graph(0)->setPen(QPen(accent, 1.4));
    p->graph(0)->setLineStyle(QCPGraph::lsLine);
    /* 组件内不显示坐标系, 只显示波形 */
    p->xAxis->setVisible(false);
    p->yAxis->setVisible(false);
    p->xAxis->grid()->setVisible(false);
    p->yAxis->grid()->setVisible(false);
    p->axisRect()->setAutoMargins(QCP::msNone);
    p->axisRect()->setMargins(QMargins(0, 0, 0, 0));
    p->setInteractions(QCP::iNone);
    p->setBackground(QBrush(p->palette().color(QPalette::Base)));
}
} // namespace

ArrayView::ArrayView(QWidget* parent) : QWidget(parent) {
    mini_ = new QCustomPlot(this);
    styleMini(mini_, accent_);
    mini_->setGeometry(0, 0, width(), height());
    mini_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mini_, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        if (menu_) menu_(mini_->mapToGlobal(p));
    });

    /* 与数值/仪表组件相同的 "—" 占位样式: 30pt 粗体 + 占位色 */
    status_ = new QLabel(mini_);
    status_->setAlignment(Qt::AlignCenter);
    status_->setAttribute(Qt::WA_TransparentForMouseEvents);
    status_->setStyleSheet("background:transparent;");
    QFont sf = font();
    sf.setPointSize(30);
    sf.setBold(true);
    status_->setFont(sf);
    status_->setGeometry(0, 0, mini_->width(), mini_->height());
    setStatus(QStringLiteral("—"), false);
}

void ArrayView::setAccent(const QColor& c) {
    accent_ = c;
    if (mini_) {
        mini_->graph(0)->setPen(QPen(accent_, 1.4));
        mini_->replot(QCustomPlot::rpQueuedReplot);
    }
    if (zoomPlot_) {
        zoomPlot_->graph(0)->setPen(QPen(accent_, 1.8));
        zoomPlot_->replot(QCustomPlot::rpQueuedReplot);
    }
}

void ArrayView::bindArray(const ArrayInfo& a) {
    array_ = a;
    values_.clear();
    if (mini_) {
        mini_->graph(0)->data()->clear();
        mini_->xAxis->setRange(0, 1);
        mini_->yAxis->setRange(0, 1);
        mini_->replot(QCustomPlot::rpQueuedReplot);
    }
    if (zoomPlot_) {
        zoomPlot_->graph(0)->data()->clear();
        zoomPlot_->replot(QCustomPlot::rpQueuedReplot);
    }
    setStatus(QStringLiteral("—"), false);
}

void ArrayView::setLive(bool on) {
    live_ = on;
    /* 不自动采样: 只更新连接状态; 无数据时保持 "—", 有数据时保留波形 */
    if (values_.isEmpty()) {
        setStatus(QStringLiteral("—"), false);
    }
}

void ArrayView::setStatus(const QString& text, bool error) {
    if (!status_) return;
    if (text.isEmpty()) {
        status_->hide();
        if (mini_) mini_->update();
        return;
    }
    QPalette pal = status_->palette();
    pal.setColor(QPalette::WindowText,
                 pal.color(error ? QPalette::BrightText : QPalette::PlaceholderText));
    if (error) pal.setColor(QPalette::WindowText, QColor("#dc2626"));
    else pal.setColor(QPalette::WindowText,
                      palette().color(QPalette::PlaceholderText));
    status_->setPalette(pal);
    status_->setText(text);
    status_->setToolTip(error ? text : QString());
    status_->setGeometry(0, 0, mini_->width(), mini_->height());
    status_->show();
}

void ArrayView::updateOnce() {
    if (!live_) {
        if (values_.isEmpty()) setStatus(QStringLiteral("—"), false);
        return;
    }
    if (!array_.isValid()) {
        setStatus(QStringLiteral("—"), false);
        return;
    }
    sampleNow();
}

void ArrayView::sampleNow() {
    if (!reader_) {
        setStatus(QStringLiteral("—"), false);
        return;
    }
    QVector<double> v;
    QString err;
    if (!reader_(array_, v, &err)) {
        setStatus(err.isEmpty() ? QStringLiteral("读取失败") : err, true);
        return;
    }
    applyValues(v);
}

void ArrayView::applyValues(const QVector<double>& v) {
    if (v.isEmpty()) {
        setStatus(QStringLiteral("—"), false);
        return;
    }
    values_ = v;
    int n = v.size();
    /* 小数组全画; 大数组按步长抽点, 保证组件内刷新流畅 */
    int step = 1;
    if (n > 4000) step = (n + 3999) / 4000;
    QVector<double> keys, vals;
    const int m = (n + step - 1) / step;
    keys.reserve(m);
    vals.reserve(m);
    for (int i = 0; i < n; i += step) {
        keys.append(i);
        vals.append(v[i]);
    }
    mini_->graph(0)->setData(keys, vals, true);
    mini_->rescaleAxes();
    mini_->replot(QCustomPlot::rpQueuedReplot);
    setStatus(QString());
    if (zoomDlg_ && zoomPlot_) updateZoom(v);
}

void ArrayView::updateZoom(const QVector<double>& v) {
    if (!zoomPlot_ || v.isEmpty()) return;
    /* 只更新数据, 不自动缩放: 保留用户当前的坐标轴范围 */
    zoomPlot_->graph(0)->setData(indexKeys(v.size()), v, true);
    zoomPlot_->replot(QCustomPlot::rpQueuedReplot);
}

void ArrayView::openZoom() {
    if (!array_.isValid()) {
        QToolTip::showText(mapToGlobal(QPoint(width() / 2, height() / 2)),
                           QStringLiteral("请先设置数组变量"), this);
        return;
    }
    if (zoomDlg_) {
        zoomDlg_->raise();
        zoomDlg_->activateWindow();
        return;
    }

    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowFlags(Qt::Window);
    const QString typeText = array_.typeText.isEmpty() ? QStringLiteral("数组")
                                                       : array_.typeText;
    dlg->setWindowTitle(QStringLiteral("%1(%2)").arg(array_.path, typeText));
    dlg->resize(900, 600);

    auto* lay = new QVBoxLayout(dlg);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);

    zoomPlot_ = new QCustomPlot(dlg);
    zoomPlot_->addGraph();
    zoomPlot_->graph(0)->setPen(QPen(accent_, 1.8));
    zoomPlot_->xAxis->setLabel(QStringLiteral("Element"));
    zoomPlot_->yAxis->setLabel(QStringLiteral("Value"));
    {
        QFont labelFont = zoomPlot_->xAxis->labelFont();
        labelFont.setPointSize(12);
        zoomPlot_->xAxis->setLabelFont(labelFont);
        zoomPlot_->yAxis->setLabelFont(labelFont);
    }
    zoomPlot_->xAxis->setNumberFormat("f");
    zoomPlot_->xAxis->setNumberPrecision(0);
    zoomPlot_->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    zoomPlot_->axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);
    zoomPlot_->axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
    zoomPlot_->setBackground(QBrush(zoomPlot_->palette().color(QPalette::Base)));
    lay->addWidget(zoomPlot_, 1);

    auto* bar = new QHBoxLayout();
    auto* hint = new QLabel(QStringLiteral("滚轮缩放；按住左键拖动（X、Y 轴均可）"), dlg);
    hint->setStyleSheet("color:#6b7280;");
    bar->addWidget(hint);
    bar->addStretch(1);
    auto* autoBtn = new QPushButton(QStringLiteral("自动缩放"), dlg);
    auto* updateBtn = new QPushButton(QStringLiteral("更新"), dlg);
    auto* closeBtn = new QPushButton(QStringLiteral("关闭"), dlg);
    bar->addWidget(autoBtn);
    bar->addWidget(updateBtn);
    bar->addWidget(closeBtn);
    lay->addLayout(bar);

    connect(autoBtn, &QPushButton::clicked, this, [this] {
        if (zoomPlot_) {
            zoomPlot_->rescaleAxes();
            zoomPlot_->replot();
        }
    });
    connect(updateBtn, &QPushButton::clicked, this, [this] { updateOnce(); });
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::close);
    /* finished 在子控件析构前发出; 先断开放大曲线, 避免访问已删除对象 */
    connect(dlg, &QDialog::finished, this, [this] {
        zoomPlot_ = nullptr;
    });
    connect(dlg, &QObject::destroyed, this, [this] {
        zoomDlg_ = nullptr;
    });

    zoomDlg_ = dlg;
    dlg->show();
    if (!values_.isEmpty()) {
        zoomPlot_->graph(0)->setData(indexKeys(values_.size()), values_, true);
        zoomPlot_->rescaleAxes();   /* 首次打开适配一次; 之后只有"自动缩放"会重设 */
        zoomPlot_->replot();
    }
}

void ArrayView::contextMenuEvent(QContextMenuEvent* e) {
    if (menu_) menu_(e->globalPos());
    else e->ignore();
}

void ArrayView::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (mini_) mini_->setGeometry(0, 0, width(), height());
    if (status_) status_->setGeometry(0, 0, mini_->width(), mini_->height());
}
