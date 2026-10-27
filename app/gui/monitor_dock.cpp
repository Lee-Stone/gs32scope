#include "monitor_dock.h"
#include <QAction>
#include <QCursor>
#include <QFont>
#include <QHeaderView>
#include <QMenu>
#include <QSet>
#include <functional>

namespace {
enum { ColName = 0, ColType = 1, ColAddr = 2, ColSize = 3,
       ColVal = 4, ColMax = 5, ColMin = 6, ColAvg = 7 };
}

MonitorDock::MonitorDock(QWidget* parent)
    : QDockWidget("监控变量", parent) {
    tree_ = new QTreeWidget(this);
    tree_->setAlternatingRowColors(true);
    /* 地址/值等十六进制列用等宽字体对齐 */
    QFont mono = tree_->font();
    mono.setFamilies({"Consolas", "Courier New", "monospace"});
    tree_->setFont(mono);
    tree_->setColumnCount(8);
    tree_->setHeaderLabels({"变量", "类型", "地址", "大小", "当前值", "最大", "最小", "均值"});
    tree_->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree_->header()->setStretchLastSection(false);
    tree_->setColumnWidth(ColName, 170);
    tree_->setColumnWidth(ColType, 60);
    tree_->setColumnWidth(ColAddr, 86);
    tree_->setColumnWidth(ColSize, 40);
    tree_->setColumnWidth(ColVal, 86);
    tree_->setColumnWidth(ColMax, 80);
    tree_->setColumnWidth(ColMin, 80);
    tree_->setColumnWidth(ColAvg, 80);
    tree_->header()->setDefaultAlignment(Qt::AlignCenter);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested,
            this, &MonitorDock::onContextMenu);
    tree_->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_->header(), &QHeaderView::customContextMenuRequested,
            this, &MonitorDock::onHeaderMenu);
    setWidget(tree_);
    setMinimumWidth(280);
    /* 默认显示 变量|地址|当前值|最大|最小|均值; 类型/大小默认隐藏 */
    tree_->setColumnHidden(ColType, true);
    tree_->setColumnHidden(ColSize, true);
}

/* 用节点填充条目自身(根直接作为 top 节点, 无额外空容器) */
static void fillNode(QTreeWidgetItem* it, const MonitorNode* node,
                     const MonitorModel* model, int rootIdx) {
    it->setData(ColName, Qt::UserRole, rootIdx);
    it->setData(ColName, Qt::UserRole + 2, node->path);
    it->setText(ColName, node->name);
    if (node->isStruct) {
        /* 父节点无值: 显式置 -1, 否则 refresh() 里 data().toInt() 对未设置
         * 的 QVariant 返回 0, 父节点会错误显示 0 号叶子的值 */
        it->setData(ColName, Qt::UserRole + 1, -1);
        const bool isPage = node->name.contains("..");   /* 数组分页分组节点 */
        if (isPage) {
            it->setText(ColType, QString());             /* 分组不显示类型 */
            it->setText(ColAddr, QString());             /* 分组不显示地址 */
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
        it->setExpanded(false);
    } else {
        it->setText(ColType, node->def.bitOffset >= 0
            ? QString("%1:%2").arg(typeName(node->def.type)).arg(node->def.bitWidth)
            : QString::fromLatin1(typeName(node->def.type)));
        it->setText(ColAddr, QStringLiteral("0x") + QString::number(node->def.addr, 16)
                        .rightJustified(8, '0').toUpper());
        it->setText(ColSize, QString::number(node->def.size));
        it->setData(ColName, Qt::UserRole + 1, node->leafIdx);
    }
}

void MonitorDock::rebuild() {
    QSet<QString> expanded;
    for (auto* it : tree_->findItems(QString(), Qt::MatchContains | Qt::MatchRecursive))
        if (it->isExpanded()) expanded.insert(it->data(ColName, Qt::UserRole + 2).toString());
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
    for (auto* it : tree_->findItems(QString(), Qt::MatchContains | Qt::MatchRecursive))
        if (expanded.contains(it->data(ColName, Qt::UserRole + 2).toString()))
            it->setExpanded(true);
    refresh();
}

static QString fmtVal(double v, const VariableDef& d) {
    if (d.type == VariableDef::Float) return QString::number(v, 'f', 3);
    if (d.bitOffset >= 0 || d.type == VariableDef::Uint32 ||
        d.type == VariableDef::Uint16 || d.type == VariableDef::Uint8)
        return QStringLiteral("0x") + QString::number((quint64)v, 16)
                   .rightJustified(d.bitOffset >= 0 || d.type == VariableDef::Uint32 ? 8 : 4, '0')
                   .toUpper();
    return QString::number(v);
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

void MonitorDock::onContextMenu(const QPoint& pos) {
    QMenu menu(this);
    menu.addAction("添加变量…", this, [this] { emit addVariableRequested(); });
    auto* it = tree_->itemAt(pos);
    if (it) {
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
