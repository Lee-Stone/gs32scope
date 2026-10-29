#include "monitor_dock.h"
#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QVBoxLayout>
#include <functional>

namespace {
enum { ColDraw = 0, ColName = 1, ColType = 2, ColAddr = 3, ColSize = 4,
       ColVal = 5, ColMax = 6, ColMin = 7, ColAvg = 8 };

/* 复选框列: 默认样式把 indicator 贴左; 这里只画底色/选中态, 把勾选框放在单元格中心 */
class CenteredCheckDelegate : public QStyledItemDelegate
{
public:
    explicit CenteredCheckDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void paint(QPainter* p, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem o = option;
        initStyleOption(&o, index);
        const QVariant cs = index.data(Qt::CheckStateRole);
        if (!cs.isValid()) {
            QStyledItemDelegate::paint(p, option, index);
            return;
        }
        const Qt::CheckState st = static_cast<Qt::CheckState>(cs.toInt());
        o.text.clear();
        o.icon = QIcon();
        o.features &= ~QStyleOptionViewItem::HasCheckIndicator;
        QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &o, p, o.widget);

        const int s = 16;
        QStyleOptionViewItem c = o;
        c.rect = QRect(option.rect.center().x() - s / 2,
                       option.rect.center().y() - s / 2, s, s);
        c.features = QStyleOptionViewItem::None;
        c.state &= ~(QStyle::State_On | QStyle::State_Off | QStyle::State_NoChange);
        c.state |= st == Qt::Checked ? QStyle::State_On
                 : st == Qt::PartiallyChecked ? QStyle::State_NoChange
                                              : QStyle::State_Off;
        QApplication::style()->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck,
                                             &c, p, o.widget);
    }
};
} // namespace

MonitorDock::MonitorDock(QWidget* parent)
    : QDockWidget("监控变量", parent) {
    tree_ = new QTreeWidget(this);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    /* 地址/值等十六进制列用等宽字体对齐 */
    QFont mono = tree_->font();
    mono.setFamilies({"Consolas", "Courier New", "monospace"});
    tree_->setFont(mono);
    tree_->setColumnCount(9);
    tree_->setHeaderLabels({"波形", "变量", "类型", "地址", "大小",
                            "当前值", "最大", "最小", "均值"});
    tree_->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree_->header()->setStretchLastSection(false);
    tree_->setColumnWidth(ColDraw, 44);
    tree_->setColumnWidth(ColName, 150);
    tree_->setColumnWidth(ColType, 60);
    tree_->setColumnWidth(ColAddr, 90);
    tree_->setColumnWidth(ColSize, 40);
    tree_->setColumnWidth(ColVal, 100);
    tree_->setColumnWidth(ColMax, 80);
    tree_->setColumnWidth(ColMin, 80);
    tree_->setColumnWidth(ColAvg, 80);
    tree_->header()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    tree_->header()->setDefaultAlignment(Qt::AlignCenter);
    tree_->setItemDelegateForColumn(ColDraw, new CenteredCheckDelegate(tree_));
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested,
            this, &MonitorDock::onContextMenu);
    tree_->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_->header(), &QHeaderView::customContextMenuRequested,
            this, &MonitorDock::onHeaderMenu);
    connect(tree_, &QTreeWidget::itemChanged, this, &MonitorDock::onItemChanged);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* it, int col) {
        if (!it || (col != ColVal && col != ColName)) return;
        const int li = it->data(ColName, Qt::UserRole + 1).toInt();
        if (li >= 0 && li < (model_ ? model_->leafCount() : 0))
            emit writeRequested(li);
    });
    auto* container = new QWidget(this);
    auto* lay = new QVBoxLayout(container);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);
    auto* bar = new QHBoxLayout();
    bar->setSpacing(8);
    auto* addWave = new QPushButton("加入波形", container);
    auto* rmWave = new QPushButton("移出波形", container);
    auto* addVar = new QPushButton("添加变量…", container);
    addWave->setToolTip("把当前选中行(叶子/结构体整组)加入波形");
    rmWave->setToolTip("把当前选中行(叶子/结构体整组)移出波形");
    for (auto* b : {addWave, rmWave, addVar}) {
        b->setFixedHeight(28);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    connect(addWave, &QPushButton::clicked, this, [this] { toggleCurrentShown(true); });
    connect(rmWave, &QPushButton::clicked, this, [this] { toggleCurrentShown(false); });
    connect(addVar, &QPushButton::clicked, this, [this] { emit addVariableRequested(); });
    bar->addWidget(addWave, 1);
    bar->addWidget(rmWave, 1);
    bar->addWidget(addVar, 1);
    lay->addLayout(bar);
    lay->addWidget(tree_, 1);
    setWidget(container);
    setMinimumWidth(360);
    /* 默认只显示 波形|变量|地址|当前值; 其余列通过表头右键开启 */
    tree_->setColumnHidden(ColType, true);
    tree_->setColumnHidden(ColSize, true);
    tree_->setColumnHidden(ColMax, true);
    tree_->setColumnHidden(ColMin, true);
    tree_->setColumnHidden(ColAvg, true);
}

void MonitorDock::setMonitorModel(MonitorModel* model) {
    model_ = model;
    if (!model_) return;
    connect(model_, &MonitorModel::leafShownChanged, this, [this](int) {
        if (!suppressDrawSync_) syncDrawChecks();
    });
}

/* 用节点填充条目自身(根直接作为 top 节点, 无额外空容器) */
static void fillNode(QTreeWidgetItem* it, const MonitorNode* node,
                     const MonitorModel* model, int rootIdx) {
    it->setData(ColName, Qt::UserRole, rootIdx);
    it->setData(ColName, Qt::UserRole + 2, node->path);
    it->setText(ColName, node->name);
    if (node->isStruct) {
        it->setData(ColName, Qt::UserRole + 1, -1);
        const bool isPage = node->name.contains("..");
        if (isPage) {
            it->setText(ColType, QString());
            it->setText(ColAddr, QString());
        } else {
            it->setText(ColType, "struct");
            it->setText(ColAddr, QStringLiteral("0x") + QString::number(node->def.addr, 16)
                            .rightJustified(8, '0').toUpper());
        }
        it->setText(ColSize, QString::number(node->def.size));
        for (const MonitorNode* c : node->children) {
            auto* cIt = new QTreeWidgetItem(it);
            fillNode(cIt, c, model, rootIdx);
        }
        /* 父节点三态: 全显示/部分显示/全隐藏 */
        int total = 0, shown = 0;
        std::function<void(const MonitorNode*)> count = [&](const MonitorNode* n) {
            if (n->isStruct) {
                for (const MonitorNode* c : n->children) count(c);
            } else if (n->leafIdx >= 0) {
                ++total;
                if (model->leaf(n->leafIdx).shown) ++shown;
            }
        };
        count(node);
        if (total > 0) {
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(ColDraw, shown == 0 ? Qt::Unchecked
                                      : shown == total ? Qt::Checked
                                                       : Qt::PartiallyChecked);
        }
        it->setExpanded(false);
    } else {
        it->setText(ColType, node->def.bitOffset >= 0
            ? QString("%1:%2").arg(typeName(node->def.type)).arg(node->def.bitWidth)
            : QString::fromLatin1(typeName(node->def.type)));
        it->setText(ColAddr, QStringLiteral("0x") + QString::number(node->def.addr, 16)
                        .rightJustified(8, '0').toUpper());
        it->setText(ColSize, QString::number(node->def.size));
        it->setData(ColName, Qt::UserRole + 1, node->leafIdx);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(ColDraw, node->leafIdx >= 0 && model->leaf(node->leafIdx).shown
                                       ? Qt::Checked : Qt::Unchecked);
    }
    for (int c = ColDraw; c <= ColAvg; ++c)
        it->setTextAlignment(c, Qt::AlignCenter);
}

void MonitorDock::rebuild() {
    if (!model_) return;
    QSet<QString> expanded;
    for (auto* it : tree_->findItems(QString(), Qt::MatchContains | Qt::MatchRecursive))
        if (it->isExpanded()) expanded.insert(it->data(ColName, Qt::UserRole + 2).toString());
    rebuilding_ = true;
    tree_->blockSignals(true);
    tree_->clear();
    for (int ri = 0; ri < model_->rootCount(); ++ri) {
        const auto& r = model_->root(ri);
        if (!r.tree) continue;
        /* 绘制专用叶子(非监控)不进监控列表 */
        if (!r.isStruct) {
            const int li = r.leaves.isEmpty() ? -1 : r.leaves.first();
            if (li < 0 || !model_->leaf(li).monitored) continue;
        }
        auto* rootIt = new QTreeWidgetItem(tree_);
        fillNode(rootIt, r.tree, model_, ri);
        rootIt->setExpanded(expanded.contains(r.name));
    }
    tree_->blockSignals(false);
    rebuilding_ = false;
    for (auto* it : tree_->findItems(QString(), Qt::MatchContains | Qt::MatchRecursive))
        if (expanded.contains(it->data(ColName, Qt::UserRole + 2).toString()))
            it->setExpanded(true);
    syncDrawChecks();
    refresh();
}

static QString fmtVal(double v, const VariableDef& d) {
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

void MonitorDock::refresh() {
    if (!model_) return;
    const int leafCount = model_->leafCount();
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* it) {
        const int li = it->data(ColName, Qt::UserRole + 1).toInt();
        if (li >= 0 && li < leafCount) {
            const auto& lf = model_->leaf(li);
            it->setText(ColVal, lf.n ? fmtVal(lf.cur, lf.def) : "—");
            it->setText(ColMax, lf.n ? fmtVal(lf.max, lf.def) : "—");
            it->setText(ColMin, lf.n ? fmtVal(lf.min, lf.def) : "—");
            it->setText(ColAvg, lf.n ? fmtVal(lf.sum / lf.n, lf.def) : "—");
        }
        for (int i = 0; i < it->childCount(); ++i) walk(it->child(i));
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        walk(tree_->topLevelItem(i));
}

int MonitorDock::leafState(QTreeWidgetItem* item, int& total) const {
    if (!item || !model_) return 0;
    const int li = item->data(ColName, Qt::UserRole + 1).toInt();
    if (li >= 0 && li < model_->leafCount()) {
        ++total;
        return model_->leaf(li).shown ? 1 : 0;
    }
    int shown = 0;
    for (int i = 0; i < item->childCount(); ++i)
        shown += leafState(item->child(i), total);
    return shown;
}

void MonitorDock::syncDrawChecks() {
    if (!model_ || rebuilding_) return;
    QSignalBlocker block(tree_);
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* it) {
        int total = 0;
        const int shown = leafState(it, total);
        if (total > 0) {
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(ColDraw, shown == 0 ? Qt::Unchecked
                                      : shown == total ? Qt::Checked
                                                       : Qt::PartiallyChecked);
        }
        for (int i = 0; i < it->childCount(); ++i) walk(it->child(i));
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        walk(tree_->topLevelItem(i));
}

void MonitorDock::applyShown(QTreeWidgetItem* item, bool on) {
    if (!item || !model_) return;
    QVector<int> leaves;
    std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* it) {
        const int li = it->data(ColName, Qt::UserRole + 1).toInt();
        if (li >= 0 && li < model_->leafCount()) {
            leaves.append(li);
            return;
        }
        for (int i = 0; i < it->childCount(); ++i) collect(it->child(i));
    };
    collect(item);
    if (leaves.size() == 1)
        model_->setLeafShown(leaves.first(), on);
    else
        model_->setLeavesShown(leaves, on);
}

void MonitorDock::toggleCurrentShown(bool on) {
    if (!model_) return;
    auto* it = tree_->currentItem();
    if (!it) return;
    suppressDrawSync_ = true;
    applyShown(it, on);
    suppressDrawSync_ = false;
    syncDrawChecks();
}

void MonitorDock::onItemChanged(QTreeWidgetItem* item, int col) {
    if (rebuilding_ || !item || col != ColDraw || !model_) return;
    const bool on = item->checkState(ColDraw) == Qt::Checked;
    suppressDrawSync_ = true;
    applyShown(item, on);
    suppressDrawSync_ = false;
    syncDrawChecks();
}

void MonitorDock::onContextMenu(const QPoint& pos) {
    QMenu menu(this);
    menu.addAction("添加变量…", this, [this] { emit addVariableRequested(); });
    auto* it = tree_->itemAt(pos);
    if (it) {
        const int li = it->data(ColName, Qt::UserRole + 1).toInt();
        if (li >= 0 && li < (model_ ? model_->leafCount() : 0)) {
            const bool shown = model_->leaf(li).shown;
            menu.addAction(shown ? "从波形移除" : "添加到波形", this, [it, shown] {
                it->setCheckState(ColDraw, shown ? Qt::Unchecked : Qt::Checked);
            });
            menu.addAction("写入值…", this, [this, li] { emit writeRequested(li); });
        } else if (it->childCount() > 0) {
            menu.addAction("整组添加到波形", this, [it] {
                it->setCheckState(ColDraw, Qt::Checked);
            });
            menu.addAction("整组从波形移除", this, [it] {
                it->setCheckState(ColDraw, Qt::Unchecked);
            });
        }
        menu.addAction("从监控列表移除(按根)", this, [this, it] {
            QTreeWidgetItem* root = it;
            while (root->parent()) root = root->parent();
            emit removeRootRequested(root->data(ColName, Qt::UserRole).toInt());
        });
    }
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

void MonitorDock::onHeaderMenu(const QPoint& pos) {
    QMenu menu(this);
    const struct { int col; const char* text; } opts[] = {
        {ColType, "类型"}, {ColSize, "大小"},
        {ColMax, "最大值"}, {ColMin, "最小值"}, {ColAvg, "均值"},
    };
    for (const auto& o : opts) {
        auto* act = menu.addAction(o.text);
        act->setCheckable(true);
        act->setChecked(!tree_->isColumnHidden(o.col));
        connect(act, &QAction::toggled, this, [this, c = o.col](bool on) {
            tree_->setColumnHidden(c, !on);
        });
    }
    menu.exec(tree_->header()->viewport()->mapToGlobal(pos));
}
