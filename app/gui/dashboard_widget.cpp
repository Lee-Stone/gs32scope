#include "dashboard_widget.h"
#include "core/monitor_model.h"
#include "core/value_codec.h"

#include <QComboBox>
#include <QCompleter>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QIcon>
#include <QPixmap>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <functional>

namespace {
constexpr double kPi = 3.14159265358979323846;

QIcon makeZoomIcon(const QColor& c) {
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(c, 3));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(7, 7, 15, 15));
    p.drawLine(QPointF(20.5, 20.5), QPointF(27, 27));
    return QIcon(pm);
}

QString fmtNumber(double v, VariableDef::Type t, int bitWidth, int decimals) {
    if (isFloatType(t)) return QString::number(v, 'f', decimals);
    if (bitWidth > 0) {
        const int w = (bitWidth + 3) / 4;
        return QStringLiteral("0x") + QString::number(static_cast<quint64>(v), 16)
                   .rightJustified(w, '0').toUpper();
    }
    return QString::number(static_cast<qlonglong>(std::llround(v)));
}

QString fmtLeaf(double v, const VariableDef& d) {
    if (isFloatType(d.type) && d.bitOffset < 0)
        return QString::number(v, 'f', d.type == VariableDef::Double ? 3 : 2);
    if (d.bitOffset >= 0) return fmtNumber(v, d.type, d.bitWidth, 0);
    return QString::number(v, 'f', 0);
}
} // namespace

/* ---------------- 绑定变量对话框 ---------------- */
class BindDialog : public QDialog
{
public:
    BindDialog(MonitorModel* model, DashboardWidget::Kind kind, QWidget* parent,
               const QVector<ArrayInfo>& arrays = {},
               const QString& curPath = {}, const QString& curLabel = {},
               double curMin = -100.0, double curMax = 100.0)
        : QDialog(parent), kind_(kind) {
        arrays_ = arrays;
        setWindowTitle("设置");
        setMinimumWidth(420);
        auto* lay = new QVBoxLayout(this);
        auto* form = new QFormLayout();

        combo_ = new QComboBox(this);
        combo_->setMinimumWidth(300);
        combo_->setEditable(true);
        combo_->setInsertPolicy(QComboBox::NoInsert);
        combo_->setMaxVisibleItems(20);
        combo_->lineEdit()->setPlaceholderText(QStringLiteral("输入变量名搜索…"));
        if (QCompleter* comp = combo_->completer()) {
            comp->setCaseSensitivity(Qt::CaseInsensitive);
            comp->setFilterMode(Qt::MatchContains);
            comp->setCompletionMode(QCompleter::PopupCompletion);
        }
        if (kind == DashboardWidget::Kind::Array) {
            if (arrays_.isEmpty()) {
                combo_->addItem(QStringLiteral("(没有可用的数组变量，请先导入 ELF)"),
                                QString());
            }
            for (const auto& a : arrays_) {
                const QString tt = a.typeText.isEmpty() ? QStringLiteral("数组") : a.typeText;
                combo_->addItem(QStringLiteral("%1(%2)")
                                    .arg(a.path, tt),
                                a.path);
            }
        } else if (model) {
            for (int i = 0; i < model->leafCount(); ++i)
                combo_->addItem(model->leaf(i).path, model->leaf(i).path);
        }
        if (kind != DashboardWidget::Kind::Array &&
            !curPath.isEmpty() && combo_->findData(curPath) < 0)
            combo_->addItem(curPath, curPath);
        if (!curPath.isEmpty()) {
            const int idx = combo_->findData(curPath);
            if (idx >= 0) combo_->setCurrentIndex(idx);
        }
        form->addRow("监控变量:", combo_);

        label_ = new QLineEdit(curLabel, this);
        if (label_->text().isEmpty() && combo_->count() > 0)
            label_->setText(shortVarName(combo_->currentData().toString()));
        form->addRow("显示名称:", label_);

        min_ = new QDoubleSpinBox(this);
        max_ = new QDoubleSpinBox(this);
        for (auto* s : {min_, max_}) {
            s->setRange(-1e9, 1e9);
            s->setDecimals(3);
            s->setSingleStep(1.0);
        }
        min_->setValue(curMin < curMax ? curMin : (kind == DashboardWidget::Kind::Toggle ? 0 : -100));
        max_->setValue(curMin < curMax ? curMax : (kind == DashboardWidget::Kind::Toggle ? 1 : 100));
        if (kind == DashboardWidget::Kind::Toggle) {
            min_->setValue(0);
            max_->setValue(1);
        }
        form->addRow("最小值:", min_);
        form->addRow("最大值:", max_);
        const bool rangeVisible = kind == DashboardWidget::Kind::Gauge ||
                                  kind == DashboardWidget::Kind::Slider;
        min_->setVisible(rangeVisible);
        max_->setVisible(rangeVisible);
        if (auto* l = form->labelForField(min_)) l->setVisible(rangeVisible);
        if (auto* l = form->labelForField(max_)) l->setVisible(rangeVisible);

        lay->addLayout(form);

        connect(label_, &QLineEdit::textEdited, this, [this] { labelEdited_ = true; });
        connect(combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            if (!labelEdited_)
                label_->setText(shortVarName(combo_->currentData().toString()));
        });

        auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        box->button(QDialogButtonBox::Ok)->setText("确定");
        box->button(QDialogButtonBox::Cancel)->setText("取消");
        connect(box, &QDialogButtonBox::accepted, this, [this] {
            if (kind_ == DashboardWidget::Kind::Array && !arrayInfo().isValid()) {
                QMessageBox::warning(this, "未选择数组", "请选择一个可用的数组变量。");
                return;
            }
            if (combo_->currentData().toString().isEmpty()) {
                QMessageBox::warning(this, "未选择变量", "请选择一个监控变量。");
                return;
            }
            if (min_->value() >= max_->value()) {
                QMessageBox::warning(this, "范围无效", "最小值必须小于最大值。");
                return;
            }
            accept();
        });
        connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
        lay->addWidget(box);
    }

    QString path() const { return combo_->currentData().toString(); }
    QString label() const {
        const QString t = label_->text().trimmed();
        return t.isEmpty() ? shortVarName(path()) : t;
    }
    double minV() const { return min_->value(); }
    double maxV() const { return max_->value(); }

    ArrayInfo arrayInfo() const {
        const QString p = combo_->currentData().toString();
        for (const auto& a : arrays_) if (a.path == p) return a;
        return ArrayInfo();
    }

private:
    DashboardWidget::Kind kind_;
    QVector<ArrayInfo> arrays_;
    QComboBox* combo_ = nullptr;
    bool labelEdited_ = false;
    QLineEdit* label_ = nullptr;
    QDoubleSpinBox* min_ = nullptr;
    QDoubleSpinBox* max_ = nullptr;
};

/* ---------------- 画布: 网格 + 吸附参考线 ---------------- */
class DashboardCanvas : public QWidget
{
public:
    explicit DashboardCanvas(DashboardWidget* owner, QWidget* parent = nullptr)
        : QWidget(parent), owner_(owner) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Base));
        const QColor fine = palette().color(QPalette::Midlight);
        p.setPen(QPen(fine, 1));
        for (int x = 0; x < width(); x += owner_->grid_) p.drawLine(x, 0, x, height());
        for (int y = 0; y < height(); y += owner_->grid_) p.drawLine(0, y, width(), y);

        /* 统一组件单元边界, 拖拽只能落在这些单元里 */
        const QColor cell = palette().color(QPalette::Mid);
        p.setPen(QPen(cell, 1, Qt::DashLine));
        const int cw = owner_->tileW_ + owner_->gap_;
        const int ch = owner_->tileH_ + owner_->gap_;
        for (int x = owner_->margin_; x < width(); x += cw)
            p.drawLine(x, 0, x, height());
        for (int y = owner_->margin_; y < height(); y += ch)
            p.drawLine(0, y, width(), y);

        if (owner_->snapX_ >= 0) {
            p.setPen(QPen(QColor("#0a6cff"), 2, Qt::DashLine));
            p.drawRect(QRect(owner_->snapX_, owner_->snapY_,
                             owner_->tileW_ - 1, owner_->tileH_ - 1));
        }
    }

private:
    DashboardWidget* owner_;
};

/* ---------------- 单个组件 ---------------- */
class DashboardItem : public QWidget
{
public:
    DashboardItem(DashboardWidget* owner, DashboardWidget::Kind kind,
                  const QString& path, const QString& label,
                  double minV, double maxV, const QColor& accent, QWidget* parent)
        : QWidget(parent), owner_(owner), kind_(kind), path_(path), label_(label),
          minV_(minV), maxV_(maxV), accent_(accent) {
        setFixedSize(preferredSize(kind));
        setMouseTracking(true);
        setCursor(Qt::OpenHandCursor);
        setToolTip(path);
        setAccessibleName(path);
        setAccessibleDescription(label);
        value_ = minV;
        sliderValue_ = value_;
        if (kind_ == DashboardWidget::Kind::Value ||
            kind_ == DashboardWidget::Kind::Editor) {
            /* 数值 = 显示 + 写入合一 */
            edit_ = new QLineEdit(this);
            apply_ = new QPushButton("写入", this);
            edit_->setObjectName("valueEdit");
            apply_->setObjectName("valueApply");
            edit_->setPlaceholderText("输入新值后回车");
            edit_->setGeometry(14, height() - 40, width() - 88, 30);
            apply_->setGeometry(width() - 68, height() - 40, 54, 30);
            connect(apply_, &QPushButton::clicked, this, [this] { applyEdit(); });
            connect(edit_, &QLineEdit::returnPressed, this, [this] { applyEdit(); });
        }
        if (kind_ == DashboardWidget::Kind::Array) {
            arrayView_ = new ArrayView(this);
            arrayView_->setAccent(accent_);
            arrayView_->setGeometry(10, 28, width() - 20, height() - 36);
            arrayZoom_ = new QToolButton(this);
            arrayZoom_->setObjectName("arrayZoom");
            arrayZoom_->setIcon(makeZoomIcon(palette().color(QPalette::PlaceholderText)));
            arrayZoom_->setIconSize(QSize(18, 18));
            arrayZoom_->setAutoRaise(true);
            arrayZoom_->setToolTip(QStringLiteral("放大数组波形"));
            arrayZoom_->setGeometry(width() - 46, 4, 32, 24);
            connect(arrayZoom_, &QToolButton::clicked, this,
                    [this] { arrayView_->openZoom(); });
            arrayView_->setMenuHandler([this](const QPoint& p) { showItemMenu(p); });
        }
    }

    static QSize preferredSize(DashboardWidget::Kind) {
        /* 所有组件统一尺寸: 便于网格吸附/禁止重叠/视觉整齐 */
        return QSize(240, 180);
    }

    DashboardWidget::Kind kind() const { return kind_; }
    QString path() const { return path_; }
    QString label() const { return label_; }
    double minV() const { return minV_; }
    double maxV() const { return maxV_; }
    bool sliderActive() const { return sliderActive_; }

    void setBinding(const QString& path, const QString& label, double minV, double maxV) {
        path_ = path;
        label_ = label;
        minV_ = minV;
        maxV_ = maxV;
        if (maxV_ <= minV_) maxV_ = minV_ + 1.0;
        value_ = std::clamp(value_, minV_, maxV_);
        sliderValue_ = value_;
        if (edit_) {
            edit_->clear();
            edit_->setStyleSheet("");
            edit_->setPlaceholderText(QStringLiteral("输入新值后回车"));
        }
        setToolTip(path);
        setAccessibleName(path);
        setAccessibleDescription(label);
        update();
    }

    void setArrayBinding(const ArrayInfo& a, const QString& label) {
        array_ = a;
        path_ = a.path;
        label_ = label.isEmpty() ? shortVarName(a.path) : label;
        if (arrayView_) arrayView_->bindArray(a);
        setToolTip(path_);
        setAccessibleName(path_);
        setAccessibleDescription(label_);
        update();
    }
    const ArrayInfo& arrayInfo() const { return array_; }
    void setArrayLive(bool on) { if (arrayView_) arrayView_->setLive(on); }
    void refreshArray() { if (arrayView_) arrayView_->updateOnce(); }
    void setArrayReader(ArrayView::Reader r) {
        if (arrayView_) arrayView_->setReader(std::move(r));
    }

    void setTileSize(const QSize& s) {
        if (s == size()) return;
        setFixedSize(s);
        if (edit_) {
            edit_->setGeometry(14, height() - 40, width() - 88, 30);
            apply_->setGeometry(width() - 68, height() - 40, 54, 30);
        }
        if (arrayView_) arrayView_->setGeometry(10, 28, width() - 20, height() - 36);
        if (arrayZoom_) arrayZoom_->setGeometry(width() - 46, 4, 32, 24);
        update();
    }

    void setValueType(VariableDef::Type type, int bitOffset, int bitWidth) {
        type_ = type;
        bitOffset_ = bitOffset;
        bitWidth_ = bitWidth;
        integer_ = !isFloatType(type);
    }

    void setValue(double v, const QString& text, bool valid) {
        if (kind_ == DashboardWidget::Kind::Array) return;
        if (sliderActive_ && kind_ == DashboardWidget::Kind::Slider) return;
        const bool changed = (v != value_) || (text != text_) || (valid != valid_);
        value_ = v;
        text_ = text;
        valid_ = valid;
        if (kind_ == DashboardWidget::Kind::Slider) sliderValue_ = v;
        if ((kind_ == DashboardWidget::Kind::Value ||
             kind_ == DashboardWidget::Kind::Editor) && edit_ && !edit_->hasFocus())
            edit_->setPlaceholderText(valid ? text_ : QStringLiteral("输入新值后回车"));
        if (changed) update();
    }

    void setCallbacks(std::function<void(const QString&, double)> write,
                      std::function<void()> bind, std::function<void()> remove) {
        write_ = std::move(write);
        bind_ = std::move(bind);
        remove_ = std::move(remove);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF card = QRectF(rect()).adjusted(1, 1, -1, -1);
        p.setPen(QPen(palette().color(QPalette::Mid), 1));
        p.setBrush(palette().color(QPalette::Base));
        p.drawRoundedRect(card, 8, 8);
        /* 左侧强调色条 */
        p.setPen(Qt::NoPen);
        p.setBrush(accent_);
        p.drawRoundedRect(QRectF(1, 10, 4, height() - 20), 2, 2);

        QFont f = font();
        f.setPointSize(9);
        p.setFont(f);
        p.setPen(palette().color(QPalette::PlaceholderText));
        QString titleText = label_;
        if (kind_ == DashboardWidget::Kind::Array && array_.isValid()) {
            const QString tt = array_.typeText.isEmpty() ? QStringLiteral("数组")
                                                        : array_.typeText;
            titleText = QStringLiteral("%1(%2)").arg(label_, tt);
        }
        const int titleW = width() - (kind_ == DashboardWidget::Kind::Array ? 64 : 100);
        const QString title = QFontMetrics(f).elidedText(titleText, Qt::ElideRight, titleW);
        p.drawText(QRectF(14, 6, titleW, 18), Qt::AlignLeft | Qt::AlignVCenter, title);

        if (kind_ == DashboardWidget::Kind::Array) return;
        if (kind_ == DashboardWidget::Kind::Gauge) paintGauge(p);
        else if (kind_ == DashboardWidget::Kind::Slider) paintSlider(p);
        else if (kind_ == DashboardWidget::Kind::Toggle) paintToggle(p);
        else paintValue(p);   /* 数值组件: 显示 + 输入写入 */
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        if (kind_ == DashboardWidget::Kind::Slider && e->pos().y() >= height() - 44) {
            sliderActive_ = true;
            updateSlider(e->pos().x());
            return;
        }
        if (kind_ == DashboardWidget::Kind::Toggle && e->pos().y() >= 34) {
            value_ = value_ != 0 ? 0.0 : 1.0;
            text_ = value_ != 0 ? "ON" : "OFF";
            valid_ = true;
            update();
            if (write_) write_(path_, value_);
            return;
        }
        dragging_ = true;
        dragOffset_ = e->pos();
        setCursor(Qt::ClosedHandCursor);
        raise();
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        if (sliderActive_) { updateSlider(e->pos().x()); return; }
        if (!dragging_ || !parentWidget()) return;
        QPoint np = mapToParent(e->pos()) - dragOffset_;
        if (owner_) {
            np = owner_->snapPosition(np, size(), this);
        } else {
            np.setX(std::clamp(np.x(), 0, std::max(0, parentWidget()->width() - width())));
            np.setY(std::clamp(np.y(), 0, std::max(0, parentWidget()->height() - height())));
        }
        move(np);
    }

    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        if (sliderActive_) {
            sliderActive_ = false;
            value_ = sliderValue_;
            if (write_) write_(path_, value_);
            update();
            return;
        }
        dragging_ = false;
        if (owner_) owner_->clearSnapGuides();
        setCursor(Qt::OpenHandCursor);
    }

    void contextMenuEvent(QContextMenuEvent* e) override {
        showItemMenu(e->globalPos());
    }

    void showItemMenu(const QPoint& global) {
        QMenu menu(this);
        menu.addAction("设置", this, [this] { if (bind_) bind_(); });
        if (kind_ == DashboardWidget::Kind::Array)
            menu.addAction("更新", this, [this] { refreshArray(); });
        menu.addAction("移除组件", this, [this] { if (remove_) remove_(); });
        menu.exec(global);
    }

private:
    QRectF trackRect() const {
        return QRectF(14, height() - 30, width() - 28, 8);
    }

    void updateSlider(int x) {
        const QRectF tr = trackRect();
        const double f = std::clamp((x - tr.left()) / std::max(1.0, tr.width()), 0.0, 1.0);
        double v = minV_ + f * (maxV_ - minV_);
        if (integer_) v = std::round(v);
        sliderValue_ = v;
        value_ = v;
        text_ = fmtNumber(v, type_, bitOffset_ >= 0 ? bitWidth_ : 0, 2);
        valid_ = true;
        update();
    }

    void paintValue(QPainter& p) {
        QFont f = font();
        f.setPointSize(30);
        f.setBold(true);
        p.setFont(f);
        p.setPen(valid_ ? palette().color(QPalette::Text) : palette().color(QPalette::PlaceholderText));
        const QString t = valid_ ? text_ : QStringLiteral("—");
        p.drawText(QRectF(14, 28, width() - 28, std::max(24, height() - 78)),
                   Qt::AlignCenter, t);
    }

    void applyEdit() {
        if (!edit_) return;
        const QString t = edit_->text().trimmed();
        bool ok = false;
        double v = 0.0;
        if (isFloatType(type_) && bitOffset_ < 0) {
            v = t.toDouble(&ok);
        } else if (gs32::isSignedType(type_) && bitOffset_ < 0) {
            const qlonglong x = t.toLongLong(&ok, 0);
            v = double(x);
        } else {
            const qulonglong x = t.toULongLong(&ok, 0);
            v = double(x);
        }
        auto fail = [&](const QString& msg) {
            edit_->setStyleSheet("border:1px solid #ef4444;");
            QToolTip::showText(edit_->mapToGlobal(QPoint(0, edit_->height())), msg, edit_);
        };
        if (!ok) { fail("请输入合法数值(整数可用 0x)"); return; }
        if (bitOffset_ >= 0) {
            const quint64 mask = gs32::bitMask(bitWidth_);
            if (v < 0 || quint64(v) > mask) {
                fail(QString("位域宽度 %1bit, 范围 0..0x%2")
                         .arg(bitWidth_).arg(mask, 0, 16));
                return;
            }
        }
        edit_->setStyleSheet("");
        if (write_) write_(path_, v);
        edit_->clear();
        edit_->setPlaceholderText(QString::number(v, 'g', 9));
    }

    void paintGauge(QPainter& p) {
        const double cx = width() / 2.0;
        const double cy = height() * 0.60;
        const double radius = std::max(28.0, std::min(width() - 64.0, height() - 84.0) / 2.0);
        const QRectF arcRect(cx - radius, cy - radius, radius * 2, radius * 2);
        const double frac = valid_ ? std::clamp((value_ - minV_) / (maxV_ - minV_), 0.0, 1.0) : 0.0;

        QPen bg(palette().color(QPalette::Midlight), 9, Qt::SolidLine, Qt::RoundCap);
        p.setPen(bg);
        p.drawArc(arcRect, 210 * 16, -240 * 16);
        if (frac > 0.001) {
            QPen accent(accent_, 9, Qt::SolidLine, Qt::RoundCap);
            p.setPen(accent);
            p.drawArc(arcRect, 210 * 16, int(-240 * 16 * frac));
        }

        p.setPen(QPen(palette().color(QPalette::PlaceholderText), 1));
        for (int i = 0; i <= 10; ++i) {
            const double theta = (210.0 - 24.0 * i) * kPi / 180.0;
            const QPointF a(cx + radius * 0.80 * std::cos(theta),
                            cy - radius * 0.80 * std::sin(theta));
            const QPointF b(cx + radius * 0.88 * std::cos(theta),
                            cy - radius * 0.88 * std::sin(theta));
            p.drawLine(a, b);
        }

        const double theta = (210.0 - 240.0 * frac) * kPi / 180.0;
        const QPointF tip(cx + radius * 0.72 * std::cos(theta),
                          cy - radius * 0.72 * std::sin(theta));
        p.setPen(QPen(accent_, 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(cx, cy), tip);
        p.setBrush(accent_);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(cx, cy), 4, 4);

        QFont f = font();
        f.setPointSize(19);
        f.setBold(true);
        p.setFont(f);
        p.setPen(valid_ ? palette().color(QPalette::Text) : palette().color(QPalette::PlaceholderText));
        p.drawText(QRectF(0, cy + radius * 0.55, width(), 28),
                   Qt::AlignCenter, valid_ ? text_ : QStringLiteral("—"));

        f.setPointSize(8);
        f.setBold(false);
        p.setFont(f);
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(QRectF(8, height() - 20, 60, 14), Qt::AlignLeft,
                   QString::number(minV_, 'g', 4));
        p.drawText(QRectF(width() - 68, height() - 20, 60, 14), Qt::AlignRight,
                   QString::number(maxV_, 'g', 4));
    }

    void paintSlider(QPainter& p) {
        QFont f = font();
        f.setPointSize(24);
        f.setBold(true);
        p.setFont(f);
        p.setPen(valid_ ? palette().color(QPalette::Text) : palette().color(QPalette::PlaceholderText));
        p.drawText(QRectF(14, 22, width() - 28, 38), Qt::AlignLeft | Qt::AlignVCenter,
                   valid_ ? text_ : QStringLiteral("—"));

        f.setPointSize(8);
        f.setBold(false);
        p.setFont(f);
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(QRectF(14, height() - 47, 80, 12), Qt::AlignLeft,
                   QString::number(minV_, 'g', 4));
        p.drawText(QRectF(width() - 94, height() - 47, 80, 12), Qt::AlignRight,
                   QString::number(maxV_, 'g', 4));

        const QRectF tr = trackRect();
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::Midlight));
        p.drawRoundedRect(tr, 4, 4);
        const double frac = valid_ ? std::clamp((value_ - minV_) / (maxV_ - minV_), 0.0, 1.0) : 0.0;
        QRectF fill = tr;
        fill.setWidth(tr.width() * frac);
        p.setBrush(accent_);
        p.drawRoundedRect(fill, 4, 4);
        const double hx = tr.left() + tr.width() * frac;
        p.setBrush(Qt::white);
        p.setPen(QPen(accent_, 2));
        p.drawEllipse(QPointF(hx, tr.center().y()), 7, 7);
    }

    void paintToggle(QPainter& p) {
        QFont f = font();
        f.setPointSize(28);
        f.setBold(true);
        p.setFont(f);
        const bool on = valid_ && value_ != 0;
        p.setPen(on ? accent_ : palette().color(QPalette::PlaceholderText));
        p.drawText(QRectF(14, 30, width() - 104, height() - 44),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   valid_ ? (on ? "ON" : "OFF") : QStringLiteral("—"));

        const QRectF pill(width() - 82, height() - 62, 64, 32);
        p.setPen(Qt::NoPen);
        p.setBrush(on ? accent_ : palette().color(QPalette::Mid));
        p.drawRoundedRect(pill, 16, 16);
        p.setBrush(Qt::white);
        const double knobX = on ? pill.right() - 16 : pill.left() + 16;
        p.drawEllipse(QPointF(knobX, pill.center().y()), 12, 12);
    }

    ArrayView* arrayView_ = nullptr;
    QToolButton* arrayZoom_ = nullptr;
    ArrayInfo array_;
    DashboardWidget* owner_ = nullptr;
    DashboardWidget::Kind kind_;
    QString path_;
    QString label_;
    double minV_ = 0.0;
    double maxV_ = 100.0;
    QColor accent_;
    double value_ = 0.0;
    QString text_ = QStringLiteral("—");
    bool valid_ = false;
    double sliderValue_ = 0.0;
    bool sliderActive_ = false;
    bool dragging_ = false;
    QPoint dragOffset_;
    VariableDef::Type type_ = VariableDef::Float;
    int bitOffset_ = -1;
    int bitWidth_ = 0;
    bool integer_ = false;
    QLineEdit* edit_ = nullptr;
    QPushButton* apply_ = nullptr;
    std::function<void(const QString&, double)> write_;
    std::function<void()> bind_;
    std::function<void()> remove_;
};

/* ---------------- 仪表盘 ---------------- */
DashboardWidget::DashboardWidget(QWidget* parent) : QWidget(parent) {
    setObjectName("dashboard");
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    canvas_ = new DashboardCanvas(this, this);
    canvas_->setMinimumSize(300, 260);
    canvas_->setStyleSheet("border:none;");
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidget(canvas_);
    root->addWidget(scroll_, 1);

    emptyHint_ = new QLabel("通过「数据 → 添加组件」创建数值 / 仪表 / 滑块 / 开关 / 数组", canvas_);
    emptyHint_->setAlignment(Qt::AlignCenter);
    emptyHint_->setStyleSheet("background:transparent;border:none;");
    {
        QPalette ep = emptyHint_->palette();
        ep.setColor(QPalette::WindowText, ep.color(QPalette::PlaceholderText));
        emptyHint_->setPalette(ep);
    }
    emptyHint_->setAttribute(Qt::WA_TransparentForMouseEvents);
    updateEmptyHint();
}

void DashboardWidget::populateAddMenu(QMenu* menu) {
    if (!menu) return;
    menu->addAction("数值", this, [this] { addItemInteractive(Kind::Value); });
    menu->addAction("仪表", this, [this] { addItemInteractive(Kind::Gauge); });
    menu->addAction("滑块", this, [this] { addItemInteractive(Kind::Slider); });
    menu->addAction("开关", this, [this] { addItemInteractive(Kind::Toggle); });
    menu->addAction("数组", this, [this] { addItemInteractive(Kind::Array); });
}

DashboardWidget::~DashboardWidget() {
    for (auto* it : items_) delete it;
    items_.clear();
}

void DashboardWidget::clearAll() {
    for (auto* it : items_) delete it;
    items_.clear();
    clearSnapGuides();
    if (canvas_) canvas_->update();
    if (scroll_) scroll_->verticalScrollBar()->setValue(0);
    updateEmptyHint();
}

QColor DashboardWidget::accentFor(int index) const {
    static const QColor kAccents[] = {
        QColor("#0a6cff"), QColor("#0f9d58"), QColor("#f59e0b"),
        QColor("#7c3aed"), QColor("#ef4444"), QColor("#0ea5e9")};
    return kAccents[index % 6];
}

QPoint DashboardWidget::cellPos(int col, int row) const {
    return QPoint(margin_ + col * (tileW_ + gap_),
                  margin_ + row * (tileH_ + gap_));
}

int DashboardWidget::colCount() const {
    if (!canvas_) return 1;
    const int avail = canvas_->width() - 2 * margin_ + gap_;
    return std::max(1, avail / (240 + gap_));
}

int DashboardWidget::tileWidth() const {
    if (!canvas_) return 240;
    const int cols = colCount();
    const int avail = canvas_->width() - 2 * margin_;
    return std::max(160, (avail - (cols - 1) * gap_) / cols);
}

int DashboardWidget::rowCount() const {
    if (!canvas_) return 1;
    const int avail = canvas_->height() - 2 * margin_ + gap_;
    return std::max(1, avail / (tileH_ + gap_));
}

bool DashboardWidget::cellOccupied(int col, int row, DashboardItem* self) const {
    const QRect cell(cellPos(col, row), QSize(tileW_, tileH_));
    for (auto* it : items_) {
        if (it == self) continue;
        if (it->geometry().intersects(cell)) return true;
    }
    return false;
}

QPoint DashboardWidget::firstFreeCell() const {
    const int cols = colCount(), rows = rowCount();
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            if (!cellOccupied(c, r, nullptr)) return cellPos(c, r);
    return QPoint(-1, -1);
}

void DashboardWidget::layoutItemsToCells() {
    tileW_ = tileWidth();
    const int cols = colCount();
    const int neededRows = (items_.size() + cols - 1) / std::max(1, cols);
    const int minH = 2 * margin_ + neededRows * tileH_ + std::max(0, neededRows - 1) * gap_;
    if (canvas_->minimumHeight() < minH) canvas_->setMinimumHeight(minH);
    const int rows = rowCount();
    int idx = 0;
    for (auto* item : items_) {
        if (idx >= cols * rows) break;
        item->setTileSize(QSize(tileW_, tileH_));
        item->move(cellPos(idx % cols, idx / cols));
        ++idx;
    }
    canvas_->update();
}

void DashboardWidget::addItem(Kind kind, const QString& path, const QString& label,
                              double minV, double maxV, const QPoint& pos) {
    (void)pos;   /* 统一单元布局, 按添加顺序排布 */
    tileW_ = tileWidth();
    auto* item = new DashboardItem(this, kind, path, label, minV, maxV,
                                   accentFor(items_.size()), canvas_);
    item->setTileSize(QSize(tileW_, tileH_));
    item->setCallbacks(
        [this](const QString& p, double v) { emit writeRequested(p, v); },
        [this, item] { bindItem(item); },
        [this, item] { removeItem(item); });
    QPoint p = firstFreeCell();
    if (p.x() < 0) {
        /* 当前可见单元已满: 增加一行, 由 QScrollArea 滚动查看 */
        const int cols = colCount();
        const int rows = (items_.size() + 1 + cols - 1) / cols;
        canvas_->setMinimumHeight(2 * margin_ + rows * tileH_ + (rows - 1) * gap_);
        p = firstFreeCell();
    }
    item->move(p.x() >= 0 ? p : cellPos(0, 0));
    item->show();
    items_.append(item);
    updateEmptyHint();
    refresh();
}

void DashboardWidget::addArrayItem(const ArrayInfo& a, const QString& label) {
    tileW_ = tileWidth();
    auto* item = new DashboardItem(this, Kind::Array, a.path, label, 0, 1,
                                   accentFor(items_.size()), canvas_);
    item->setTileSize(QSize(tileW_, tileH_));
    item->setCallbacks(
        [this](const QString& p, double v) { emit writeRequested(p, v); },
        [this, item] { bindItem(item); },
        [this, item] { removeItem(item); });
    item->setArrayReader([this](const ArrayInfo& ai, QVector<double>& v, QString* e) {
        return sampleArray(ai, v, e);
    });
    item->setArrayBinding(a, label);
    item->setArrayLive(arrayLive_);
    QPoint p = firstFreeCell();
    if (p.x() < 0) {
        const int cols = colCount();
        const int rows = (items_.size() + 1 + cols - 1) / cols;
        canvas_->setMinimumHeight(2 * margin_ + rows * tileH_ + (rows - 1) * gap_);
        p = firstFreeCell();
    }
    item->move(p.x() >= 0 ? p : cellPos(0, 0));
    item->show();
    items_.append(item);
    updateEmptyHint();
}

void DashboardWidget::addItemInteractive(Kind kind) {
    if (kind == Kind::Array) {
        if (arrays_.isEmpty()) {
            QMessageBox::information(this, "没有可绑定的数组",
                "当前没有可用的数值型数组，请先导入包含数组变量的 ELF。");
            return;
        }
        BindDialog dlg(model_, Kind::Array, this, arrays_);
        if (dlg.exec() == QDialog::Accepted) {
            const ArrayInfo a = dlg.arrayInfo();
            if (a.isValid()) addArrayItem(a, dlg.label());
        }
        return;
    }
    if (!model_ || model_->leafCount() == 0) {
        QMessageBox::information(this, "没有可绑定变量",
            "请先在「变量…」对话框里导入 ELF/SVD 并监控变量。");
        return;
    }
    BindDialog dlg(model_, kind, this, {});
    if (dlg.exec() == QDialog::Accepted)
        addItem(kind, dlg.path(), dlg.label(), dlg.minV(), dlg.maxV());
}

void DashboardWidget::bindItem(DashboardItem* item) {
    if (!item) return;
    if (item->kind() == Kind::Array) {
        BindDialog dlg(model_, Kind::Array, this, arrays_, item->path(), item->label());
        if (dlg.exec() == QDialog::Accepted) {
            const ArrayInfo a = dlg.arrayInfo();
            if (a.isValid())
                item->setArrayBinding(a, dlg.label());
        }
        return;
    }
    BindDialog dlg(model_, item->kind(), this, {}, item->path(), item->label(),
                   item->minV(), item->maxV());
    if (dlg.exec() == QDialog::Accepted) {
        item->setBinding(dlg.path(), dlg.label(), dlg.minV(), dlg.maxV());
        refresh();
    }
}

void DashboardWidget::removeItem(DashboardItem* item) {
    if (!item) return;
    items_.removeOne(item);
    item->deleteLater();
    clearSnapGuides();
    canvas_->update();
    updateEmptyHint();
}

void DashboardWidget::setArrayReader(ArrayReader r) {
    arrayReader_ = std::move(r);
    for (auto* it : items_) it->setArrayReader(arrayReader_);
}

void DashboardWidget::setArrayLive(bool on) {
    arrayLive_ = on;
    for (auto* it : items_) it->setArrayLive(on);
}

bool DashboardWidget::sampleArray(const ArrayInfo& a, QVector<double>& out,
                                  QString* err) {
    if (!arrayReader_) {
        if (err) *err = QStringLiteral("未设置数组读取接口");
        return false;
    }
    return arrayReader_(a, out, err);
}

void DashboardWidget::refresh() {
    if (!model_) return;
    for (auto* item : items_) {
        if (item->kind() == Kind::Array) continue;
        const int li = model_->leafIndexByPath(item->path());
        if (li < 0) {
            item->setValue(0, QStringLiteral("—"), false);
            continue;
        }
        const auto& lf = model_->leaf(li);
        item->setValueType(lf.def.type, lf.def.bitOffset, lf.def.bitWidth);
        if (item->kind() == Kind::Toggle) {
            const bool on = lf.n && lf.cur != 0;
            item->setValue(on ? 1.0 : 0.0, on ? "ON" : "OFF", lf.n > 0);
        } else {
            item->setValue(lf.cur, fmtLeaf(lf.cur, lf.def), lf.n > 0);
        }
    }
}

void DashboardWidget::createDemoPanel() {
    clearAll();
    addItem(Kind::Gauge, "sim.speed", "转速 (rpm)", 0, 120);
    addItem(Kind::Gauge, "sim.current", "电流 (A)", -5, 5);
    addItem(Kind::Slider, "sim.target", "目标转速 (rpm)", 0, 120);
    addItem(Kind::Toggle, "sim.enable", "使能", 0, 1);
    addItem(Kind::Value, "sim.temp", "温度 (℃)", 0, 100);
    layoutItemsToCells();
    updateEmptyHint();
    refresh();
    if (scroll_) scroll_->verticalScrollBar()->setValue(0);
}

void DashboardWidget::saveState(QSettings& s) const {
    s.beginWriteArray("dashboard/items", items_.size());
    for (int i = 0; i < items_.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue("kind", int(items_[i]->kind()));
        s.setValue("path", items_[i]->path());
        s.setValue("label", items_[i]->label());
        s.setValue("min", items_[i]->minV());
        s.setValue("max", items_[i]->maxV());
        s.setValue("x", items_[i]->x());
        s.setValue("y", items_[i]->y());
        if (items_[i]->kind() == Kind::Array) {
            const ArrayInfo a = items_[i]->arrayInfo();
            s.setValue("addr", qulonglong(a.addr));
            s.setValue("count", a.count);
            s.setValue("elemSize", a.elemSize);
            s.setValue("type", int(a.type));
            s.setValue("bitOffset", a.bitOffset);
            s.setValue("bitWidth", a.bitWidth);
            s.setValue("typeText", a.typeText);
        }
    }
    s.endArray();
}

void DashboardWidget::restoreState(QSettings& s) {
    clearAll();
    const int n = s.beginReadArray("dashboard/items");
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        int k = s.value("kind", -1).toInt();
        if (k == int(Kind::Array)) {
            ArrayInfo a;
            a.path = s.value("path").toString();
            a.addr = s.value("addr").toUInt();
            a.count = s.value("count").toUInt();
            a.elemSize = s.value("elemSize").toUInt();
            a.type = VariableDef::Type(s.value("type", int(VariableDef::Float)).toInt());
            a.bitOffset = s.value("bitOffset", -1).toInt();
            a.bitWidth = s.value("bitWidth", 0).toInt();
            a.typeText = s.value("typeText").toString();
            for (const auto& ca : arrays_) {
                if (ca.path == a.path && ca.count == a.count && ca.type == a.type) {
                    a = ca;
                    break;
                }
            }
            if (a.isValid())
                addArrayItem(a, s.value("label").toString());
            continue;
        }
        if (k < int(Kind::Value) || k > int(Kind::Editor)) continue;
        if (k == int(Kind::Editor)) k = int(Kind::Value);   /* 旧“数值编辑”合并到“数值” */
        addItem(Kind(k), s.value("path").toString(), s.value("label").toString(),
                s.value("min", -100).toDouble(), s.value("max", 100).toDouble());
    }
    s.endArray();
    layoutItemsToCells();
    updateEmptyHint();
    refresh();
    if (scroll_) scroll_->verticalScrollBar()->setValue(0);
}

QPoint DashboardWidget::snapPosition(const QPoint& proposed, const QSize& size,
                                     DashboardItem* self) {
    (void)size;
    const int cols = colCount(), rows = rowCount();
    if (cols <= 0 || rows <= 0) return self->pos();
    int c = int(std::lround(double(proposed.x() - margin_) / (tileW_ + gap_)));
    int r = int(std::lround(double(proposed.y() - margin_) / (tileH_ + gap_)));
    c = std::clamp(c, 0, cols - 1);
    r = std::clamp(r, 0, rows - 1);

    /* 找离鼠标最近且未被占用的单元; 找不到就留在原位置 */
    int bestC = -1, bestR = -1, bestDist = INT_MAX;
    for (int rr = 0; rr < rows; ++rr) {
        for (int cc = 0; cc < cols; ++cc) {
            if (cellOccupied(cc, rr, self)) continue;
            const int d = (cc - c) * (cc - c) + (rr - r) * (rr - r);
            if (d < bestDist) { bestDist = d; bestC = cc; bestR = rr; }
        }
    }
    if (bestC < 0) return self->pos();
    const QPoint p = cellPos(bestC, bestR);
    snapX_ = p.x();
    snapY_ = p.y();
    canvas_->update();
    return p;
}

void DashboardWidget::clearSnapGuides() {
    snapX_ = snapY_ = -1;
    canvas_->update();
}

void DashboardWidget::updateEmptyHint() {
    if (!emptyHint_ || !canvas_) return;
    emptyHint_->setVisible(items_.isEmpty());
    emptyHint_->setGeometry(canvas_->rect());
    emptyHint_->lower();
}

void DashboardWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (!canvas_) return;
    /* 仅在列数/格子宽度变化时重排; 用户拖拽后的位置在切页时保持 */
    const int cols = colCount();
    const int tw = tileWidth();
    if (!items_.isEmpty() && (cols != lastCols_ || tw != tileW_)) {
        lastCols_ = cols;
        tileW_ = tw;
        layoutItemsToCells();
    }
    updateEmptyHint();
}

void DashboardWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (!canvas_) return;
    const int cols = colCount();
    const int tw = tileWidth();
    if (cols != lastCols_ || tw != tileW_) {
        lastCols_ = cols;
        tileW_ = tw;
        layoutItemsToCells();
    }
    updateEmptyHint();
}
