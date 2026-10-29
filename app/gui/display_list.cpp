#include "display_list.h"
#include <QApplication>
#include <QHeaderView>
#include <QMenu>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>

static QString fmtStat(double v, const VariableDef& d) {
    if (d.type == VariableDef::Double) return QString::number(v, 'f', 6);
    if (d.type == VariableDef::Float) return QString::number(v, 'f', 3);
    if (d.bitOffset >= 0) {
        const int w = d.bitWidth > 0 ? (d.bitWidth + 3) / 4 : 8;
        return QStringLiteral("0x") + QString::number((quint64)v, 16)
                   .rightJustified(w, '0').toUpper();
    }
    switch (d.type) {
    case VariableDef::Uint8:
    case VariableDef::Uint16:
    case VariableDef::Uint32:
        return QStringLiteral("0x") + QString::number((quint64)v, 16)
                   .rightJustified(d.size * 2, '0').toUpper();
    case VariableDef::Uint64:
        return QStringLiteral("0x") + QString::number((quint64)v, 16)
                   .rightJustified(16, '0').toUpper();
    default:
        return QString::number(v, 'f', 0);
    }
}

/* 颜色列: 色块水平/垂直居中, 不依赖 icon 左对齐 */
class ColorDelegate : public QStyledItemDelegate
{
public:
    explicit ColorDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}
    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& idx) const override {
        QStyleOptionViewItem o = opt;
        initStyleOption(&o, idx);
        o.text.clear();
        o.icon = QIcon();
        QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &o, p, o.widget);
        const QColor c = idx.data(Qt::DecorationRole).value<QColor>();
        if (!c.isValid()) return;
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        const int s = 15;
        const QRectF r(opt.rect.center().x() - s / 2.0,
                       opt.rect.center().y() - s / 2.0, s, s);
        p->setPen(QPen(c.darker(135), 1));
        p->setBrush(c);
        p->drawRoundedRect(r, 3, 3);
        p->restore();
    }
};

DisplayList::DisplayList(QWidget* parent)
    : QTableWidget(parent) {
    setColumnCount(9);
    setHorizontalHeaderLabels({"颜色", "变量", "类型", "地址", "大小",
                               "当前值", "最大", "最小", "平均"});
    horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);   /* 变量列吸收剩余宽度, 铺满底部 */
    horizontalHeader()->setStretchLastSection(false);
    setColumnWidth(ColColor, 52);
    setColumnWidth(ColName, 160);
    setColumnWidth(ColType, 60);
    setColumnWidth(ColAddr, 86);
    setColumnWidth(ColSize, 40);
    setColumnWidth(ColCur, 90);
    setColumnWidth(ColMax, 80);
    setColumnWidth(ColMin, 80);
    setColumnWidth(ColAvg, 80);
    verticalHeader()->setVisible(false);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setItemDelegateForColumn(ColColor, new ColorDelegate(this));
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QTableWidget::customContextMenuRequested,
            this, &DisplayList::onContextMenu);
}

void DisplayList::rebuild() {
    rebuilding_ = true;
    clearContents();
    setRowCount(0);
    int row = 0;
    for (int li = 0; li < model_->leafCount(); ++li) {
        const auto& lf = model_->leaf(li);
        if (!lf.shown) continue;
        insertRow(row);
        auto* colorIt = new QTableWidgetItem();
        colorIt->setData(Qt::DecorationRole, model_->root(lf.rootIdx).color);
        colorIt->setFlags(Qt::ItemIsEnabled);
        setItem(row, ColColor, colorIt);

        auto* nameIt = new QTableWidgetItem(lf.path);
        nameIt->setData(Qt::UserRole, li);
        nameIt->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        setItem(row, ColName, nameIt);
        setItem(row, ColType, new QTableWidgetItem(
            lf.def.bitOffset >= 0
                ? QString("%1:%2").arg(typeName(lf.def.type)).arg(lf.def.bitWidth)
                : QString::fromLatin1(typeName(lf.def.type))));
        auto* addrIt = new QTableWidgetItem(
            QStringLiteral("0x%1").arg(lf.def.addr, 8, 16, QChar('0')));
        addrIt->setFlags(Qt::ItemIsEnabled);
        setItem(row, ColAddr, addrIt);
        auto* sizeIt = new QTableWidgetItem(QString::number(lf.def.size));
        sizeIt->setFlags(Qt::ItemIsEnabled);
        setItem(row, ColSize, sizeIt);
        setItem(row, ColCur, new QTableWidgetItem(
            lf.n ? fmtStat(lf.cur, lf.def) : "—"));
        setItem(row, ColMax, new QTableWidgetItem(
            lf.n ? fmtStat(lf.max, lf.def) : "—"));
        setItem(row, ColMin, new QTableWidgetItem(
            lf.n ? fmtStat(lf.min, lf.def) : "—"));
        setItem(row, ColAvg, new QTableWidgetItem(
            lf.n ? fmtStat(lf.sum / lf.n, lf.def) : "—"));
        for (int c = 0; c < columnCount(); ++c)
            if (auto* it = item(row, c)) it->setTextAlignment(Qt::AlignCenter);
        ++row;
    }
    rebuilding_ = false;
}

void DisplayList::refreshStats() {
    if (!model_ || rebuilding_) return;
    for (int row = 0; row < rowCount(); ++row) {
        const auto* nameIt = item(row, ColName);
        if (!nameIt) continue;
        const int li = nameIt->data(Qt::UserRole).toInt();
        if (li < 0 || li >= model_->leafCount()) continue;
        const auto& lf = model_->leaf(li);
        if (!lf.shown) continue;
        if (auto* it = item(row, ColCur)) it->setText(lf.n ? fmtStat(lf.cur, lf.def) : "—");
        if (auto* it = item(row, ColMax)) it->setText(lf.n ? fmtStat(lf.max, lf.def) : "—");
        if (auto* it = item(row, ColMin)) it->setText(lf.n ? fmtStat(lf.min, lf.def) : "—");
        if (auto* it = item(row, ColAvg)) it->setText(lf.n ? fmtStat(lf.sum / lf.n, lf.def) : "—");
    }
}

void DisplayList::onContextMenu(const QPoint& pos) {
    QMenu menu(this);
    menu.addAction("添加波形变量…", this, [this] { emit addVariableRequested(); });
    auto* item = itemAt(pos);
    if (!item) {
        menu.exec(viewport()->mapToGlobal(pos));
        return;
    }
    const int row = item->row();
    const QTableWidgetItem* nameIt = this->item(row, ColName);
    if (!nameIt) return;
    const int li = nameIt->data(Qt::UserRole).toInt();
    menu.addSeparator();
    menu.addAction("从显示移除", this, [this, li] {
        model_->setLeafShown(li, false);
    });
    menu.exec(viewport()->mapToGlobal(pos));
}
