#include "display_list.h"
#include <QHeaderView>
#include <QIcon>
#include <QMenu>
#include <QPixmap>

static QString fmtStat(double v, const VariableDef& d) {
    if (d.type == VariableDef::Float) return QString::number(v, 'f', 3);
    if (d.bitOffset >= 0 || d.type == VariableDef::Uint32 ||
        d.type == VariableDef::Uint16 || d.type == VariableDef::Uint8)
        return QStringLiteral("0x%1").arg((quint64)v, d.bitOffset >= 0 || d.type == VariableDef::Uint32 ? 8 : 4, 16, QChar('0'));
    return QString::number(v);
}

DisplayList::DisplayList(QWidget* parent)
    : QTableWidget(parent) {
    setColumnCount(9);
    setHorizontalHeaderLabels({"颜色", "变量", "类型", "地址", "大小",
                               "最大", "最小", "Y Div", "Y Offset"});
    horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    horizontalHeader()->setStretchLastSection(false);
    setColumnWidth(ColColor, 30);
    setColumnWidth(ColName, 160);
    setColumnWidth(ColType, 60);
    setColumnWidth(ColAddr, 86);
    setColumnWidth(ColSize, 40);
    setColumnWidth(ColMax, 80);
    setColumnWidth(ColMin, 80);
    setColumnWidth(ColDiv, 60);
    setColumnWidth(ColOff, 60);
    verticalHeader()->setVisible(false);
    setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QTableWidget::itemChanged, this, &DisplayList::onItemChanged);
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
        QPixmap pm(12, 12);
        pm.fill(model_->root(lf.rootIdx).color);
        auto* colorIt = new QTableWidgetItem(QIcon(pm), QString());
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
        setItem(row, ColMax, new QTableWidgetItem(
            lf.n ? fmtStat(lf.max, lf.def) : "—"));
        setItem(row, ColMin, new QTableWidgetItem(
            lf.n ? fmtStat(lf.min, lf.def) : "—"));
        auto* divIt = new QTableWidgetItem("Auto");
        divIt->setData(Qt::UserRole + 1, li);
        divIt->setFlags(Qt::ItemIsEnabled | Qt::ItemIsEditable);
        setItem(row, ColDiv, divIt);
        auto* offIt = new QTableWidgetItem("0");
        offIt->setData(Qt::UserRole + 1, li);
        offIt->setFlags(Qt::ItemIsEnabled | Qt::ItemIsEditable);
        setItem(row, ColOff, offIt);
        ++row;
    }
    rebuilding_ = false;
}

void DisplayList::onItemChanged(QTableWidgetItem* item) {
    if (rebuilding_ || !item) return;
    const int col = item->column();
    if (col != ColDiv && col != ColOff) return;
    const int li = item->data(Qt::UserRole + 1).toInt();
    if (li < 0 || li >= model_->leafCount()) return;
    const QString txt = item->text().trimmed();
    bool ok = false;
    double v = 0;
    if (txt.compare("auto", Qt::CaseInsensitive) == 0) {
        emit channelParamChanged(model_->leaf(li).path, 1.0, 0.0, true);
        return;
    }
    v = txt.toDouble(&ok);
    if (!ok) { item->setText("Auto"); return; }
    const double div = model_->leaf(li).def.type == VariableDef::Float ? v : v;
    emit channelParamChanged(model_->leaf(li).path, col == ColDiv ? v : div,
                             col == ColOff ? v : 0.0, false);
}

void DisplayList::onContextMenu(const QPoint& pos) {
    auto* item = itemAt(pos);
    if (!item) return;
    const int row = item->row();
    const QTableWidgetItem* nameIt = this->item(row, ColName);
    if (!nameIt) return;
    const int li = nameIt->data(Qt::UserRole).toInt();
    QMenu menu(this);
    menu.addAction("从显示移除", this, [this, li] {
        model_->setLeafShown(li, false);
    });
    menu.exec(viewport()->mapToGlobal(pos));
}
