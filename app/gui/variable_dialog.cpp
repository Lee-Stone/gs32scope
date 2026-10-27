#include "variable_dialog.h"
#include "symbols/elf_parser.h"
#include "symbols/svd_parser.h"
#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>
#include <functional>

namespace {   /* 与 VariableDialog 私有枚举一致 */
enum { ColName = 0, ColAddr = 1, ColSize = 2, ColType = 3, ColDraw = 4 };

/* 名称比较: 字母从小到大; 同一字母小写优先于大写(如 a<A, ab<Ab) */
static int charRank(QChar c) {
    if (c.isLower()) return c.toUpper().unicode() * 2;
    if (c.isUpper()) return c.toUpper().unicode() * 2 + 1;
    return c.unicode();
}
static bool lessName(const QString& a, const QString& b) {
    const int n = qMin(a.size(), b.size());
    for (int i = 0; i < n; ++i) {
        const int ra = charRank(a[i]), rb = charRank(b[i]);
        if (ra != rb) return ra < rb;
    }
    return a.size() < b.size();
}

static quint64 nodeAddr(QTreeWidgetItem* it) {
    return it->data(ColName, Qt::UserRole).value<VariableDef>().addr;
}

/* 顶层(文件根下)变量比较: 名称列字母序(小写优先), 地址/大小数值序, 其余文本序 */
static int compareTop(QTreeWidgetItem* a, QTreeWidgetItem* b, int col) {
    switch (col) {
    case ColAddr: {
        const quint64 va = nodeAddr(a), vb = nodeAddr(b);
        return va < vb ? -1 : (va > vb ? 1 : 0);
    }
    case ColSize: {
        const int sa = a->text(col).toInt(), sb = b->text(col).toInt();
        return sa < sb ? -1 : (sa > sb ? 1 : 0);
    }
    case ColName:
        if (lessName(a->text(col), b->text(col))) return -1;
        if (lessName(b->text(col), a->text(col))) return 1;
        return 0;
    default:
        return QString::compare(a->text(col), b->text(col));
    }
}

/* 源文件分组行: 整行绘制 */
class FileGroupDelegate : public QStyledItemDelegate {
public:
    explicit FileGroupDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}
    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& idx) const override {
        const bool isGroup = idx.data(Qt::UserRole + 2).toBool();
        if (!isGroup || idx.column() != 0) {
            QStyledItemDelegate::paint(p, opt, idx);
            return;
        }
        QStyleOptionViewItem o = opt;
        initStyleOption(&o, idx);
        if (const QWidget* w = o.widget)
            o.rect.setRight(w->width() - 1);   /* 扩展绘制区到整行宽 */
        const QStyle* st = o.widget ? o.widget->style() : QApplication::style();
        st->drawControl(QStyle::CE_ItemViewItem, &o, p, o.widget);
    }
};

/* 内容居中列(地址/大小): 覆盖文本对齐 */
class AlignCenterDelegate : public QStyledItemDelegate {
public:
    explicit AlignCenterDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}
    void initStyleOption(QStyleOptionViewItem* o, const QModelIndex& idx) const override {
        QStyledItemDelegate::initStyleOption(o, idx);
        o->displayAlignment = Qt::AlignCenter;
    }
};

/* 绘制列: 复选框在单元格内水平居中 */
class CheckCenterDelegate : public QStyledItemDelegate {
public:
    explicit CheckCenterDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}
    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& idx) const override {
        QStyleOptionViewItem o = opt;
        initStyleOption(&o, idx);
        o.decorationPosition = QStyleOptionViewItem::Right;   /* 图标区放右侧 */
        o.decorationAlignment = Qt::AlignCenter;
        QStyledItemDelegate::paint(p, o, idx);
    }
};
}

VariableDialog::VariableDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("变量选择");
    resize(1024, 768);
    setMinimumSize(560, 480);
    setModal(false);

    auto* lay = new QVBoxLayout(this);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText("搜索变量名…");
    connect(search_, &QLineEdit::textChanged, this, &VariableDialog::onSearch);
    lay->addWidget(search_);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(5);
    tree_->setHeaderLabels({"变量", "地址", "大小", "类型", "绘制"});
    tree_->setAlternatingRowColors(true);   /* 表格交替行色 */
    tree_->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree_->header()->setDefaultAlignment(Qt::AlignCenter);   /* 表头居中 */
    QFont hdrFont = tree_->header()->font();
    hdrFont.setBold(true);                                    /* 表头加粗 */
    tree_->header()->setFont(hdrFont);
    tree_->setColumnWidth(ColName, 300);
    tree_->setColumnWidth(ColAddr, 100);
    tree_->setColumnWidth(ColSize, 80);
    tree_->setColumnWidth(ColType, 200);
    tree_->setColumnWidth(ColDraw, 80);
    tree_->header()->setStretchLastSection(false);
    /* 地址/大小列内容居中; 绘制列复选框居中 */
    tree_->setItemDelegateForColumn(ColAddr, new AlignCenterDelegate(tree_));
    tree_->setItemDelegateForColumn(ColSize, new AlignCenterDelegate(tree_));
    tree_->setItemDelegateForColumn(ColDraw, new CheckCenterDelegate(tree_));
    /* 内建排序会递归重排/翻转结构体成员, 禁用;
     * 改为表头点击只排顶层(文件根下), 成员保持定义顺序(地址从小到大) */
    tree_->setSortingEnabled(false);
    tree_->header()->setSectionsClickable(true);
    tree_->header()->setSortIndicatorShown(true);
    connect(tree_->header(), &QHeaderView::sortIndicatorChanged,
            this, &VariableDialog::onSortByColumn);
    connect(tree_, &QTreeWidget::itemChanged, this, &VariableDialog::onItemChanged);
    tree_->setItemDelegate(new FileGroupDelegate(tree_));   /* 源文件分组整行绘制 */
    lay->addWidget(tree_, 1);

    auto* row = new QHBoxLayout();
    auto* add = new QPushButton("添加…", this);
    auto* del = new QPushButton("删除", this);
    auto* collapse = new QPushButton("折叠变量", this);
    auto* expand = new QPushButton("展开变量", this);
    add->setFixedSize(100, 30);
    del->setFixedSize(100, 30);
    collapse->setFixedSize(100, 30);
    expand->setFixedSize(100, 30);
    auto* menu = new QMenu(add);
    menu->addAction(QStringLiteral("从 ELF 导入变量…"), this, &VariableDialog::onImportElf);
    menu->addAction(QStringLiteral("从 SVD 导入寄存器…"), this, &VariableDialog::onImportSvd);
    add->setMenu(menu);
    connect(del, &QPushButton::clicked, this, &VariableDialog::onRemoveSelected);
    /* 折叠/展开变量节点: ELF 根与源文件分组行保持展开,
     * 只收起/展开分组下的变量(结构体)节点及其子树 */
    auto applyVarExpand = [this](bool expanded) {
        for (int t = 0; t < tree_->topLevelItemCount(); ++t) {
            auto* fileRoot = tree_->topLevelItem(t);
            for (int i = 0; i < fileRoot->childCount(); ++i) {
                auto* g = fileRoot->child(i);
                if (g->data(ColName, Qt::UserRole + 1).toInt() == 1) {
                    for (int k = 0; k < g->childCount(); ++k)
                        g->child(k)->setExpanded(expanded);
                } else {
                    g->setExpanded(expanded);   /* 文件根下直接挂的变量 */
                }
            }
        }
    };
    connect(collapse, &QPushButton::clicked, this, [applyVarExpand] { applyVarExpand(false); });
    connect(expand, &QPushButton::clicked, this, [applyVarExpand] { applyVarExpand(true); });
    row->addWidget(add);
    row->addWidget(del);
    row->addSpacing(20);            /* 折叠/展开与删除按钮隔开 20px */
    row->addWidget(collapse);
    row->addWidget(expand);
    row->addStretch(1);
    lay->addLayout(row);
}

void VariableDialog::sortTopLevel(int column, Qt::SortOrder order) {
    /* 重挂会丢失展开状态, 先记录所有已展开节点 */
    QSet<QTreeWidgetItem*> expanded;
    std::function<void(QTreeWidgetItem*)> collectExpanded =
        [&](QTreeWidgetItem* it) {
        if (it->isExpanded()) expanded.insert(it);
        for (int i = 0; i < it->childCount(); ++i) collectExpanded(it->child(i));
    };
    for (int t = 0; t < tree_->topLevelItemCount(); ++t)
        collectExpanded(tree_->topLevelItem(t));

    tree_->blockSignals(true);   /* 重挂不触发 itemChanged */
    for (int t = 0; t < tree_->topLevelItemCount(); ++t) {
        auto* fileRoot = tree_->topLevelItem(t);
        /* 第一级: 文件根下的直接子项(源文件分组/外设)按名称排, 不跨分组 */
        QVector<QTreeWidgetItem*> groups;
        groups.reserve(fileRoot->childCount());
        for (int i = 0; i < fileRoot->childCount(); ++i)
            groups.append(fileRoot->child(i));
        std::stable_sort(groups.begin(), groups.end(),
            [&](QTreeWidgetItem* a, QTreeWidgetItem* b) {
                const int c = compareTop(a, b, ColName);
                return order == Qt::AscendingOrder ? c < 0 : c > 0;
            });
        for (int i = 0; i < groups.size(); ++i) {
            fileRoot->removeChild(groups[i]);
            fileRoot->insertChild(i, groups[i]);
        }
        /* 第二级: 源文件分组内的变量按当前列排序(只在同一文件内, 不跨文件) */
        for (auto* g : groups) {
            if (g->data(ColName, Qt::UserRole + 1).toInt() != 1) continue;
            QVector<QTreeWidgetItem*> vars;
            vars.reserve(g->childCount());
            for (int i = 0; i < g->childCount(); ++i)
                vars.append(g->child(i));
            std::stable_sort(vars.begin(), vars.end(),
                [&](QTreeWidgetItem* a, QTreeWidgetItem* b) {
                    const int c = compareTop(a, b, column);
                    return order == Qt::AscendingOrder ? c < 0 : c > 0;
                });
            for (int i = 0; i < vars.size(); ++i) {
                g->removeChild(vars[i]);
                g->insertChild(i, vars[i]);
            }
        }
    }
    tree_->blockSignals(false);
    /* 恢复展开状态 */
    for (QTreeWidgetItem* it : expanded) it->setExpanded(true);
    /* 重挂会清掉 firstColumnSpanned, 恢复源文件分组整行显示(放信号解锁后) */
    for (int t = 0; t < tree_->topLevelItemCount(); ++t) {
        auto* fileRoot = tree_->topLevelItem(t);
        for (int i = 0; i < fileRoot->childCount(); ++i) {
            auto* c = fileRoot->child(i);
            if (c->data(ColName, Qt::UserRole + 1).toInt() == 1)
                c->setFirstColumnSpanned(true);
        }
    }
}

void VariableDialog::onSortByColumn(int column, Qt::SortOrder order) {
    userSorted_ = true;   /* 表头排序只来自用户点击(导入路径直接调 sortTopLevel) */
    sortTopLevel(column, order);   /* 只排顶层; 结构体成员始终是定义顺序 */
}

QTreeWidgetItem* VariableDialog::ensureFileRoot(const QString& file) {
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        if (tree_->topLevelItem(i)->text(ColName) == file)
            return tree_->topLevelItem(i);
    auto* root = new QTreeWidgetItem(tree_);
    root->setText(ColName, file);
    root->setFirstColumnSpanned(true);
    return root;   /* 文件根无勾选框 */
}

void VariableDialog::addLeafItem(QTreeWidgetItem* parent, const VariableDef& v) {
    auto* it = new QTreeWidgetItem(parent);
    it->setText(ColName, shortVarName(v.name));
    it->setData(ColName, Qt::UserRole, QVariant::fromValue(v));
    it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
    it->setCheckState(ColName, Qt::Unchecked);   /* 节点勾选=监控 */
    it->setCheckState(ColDraw, Qt::Unchecked);   /* 绘制列(仅叶子) */
    it->setText(ColAddr, "0x" + QString::number(v.addr, 16).rightJustified(8, '0').toUpper());
    it->setText(ColSize, QString::number(v.size));
    it->setText(ColType, v.bitOffset >= 0
        ? QString("%1:%2").arg(typeName(v.type)).arg(v.bitWidth)
        : QString::fromLatin1(typeName(v.type)));
}

QString VariableDialog::itemPath(QTreeWidgetItem* it) const {
    QStringList parts;
    for (QTreeWidgetItem* p = it; p && p->parent(); p = p->parent())
        parts.prepend(p->text(ColName));
    return parts.join('.');
}

DwarfVar VariableDialog::toDwarf(QTreeWidgetItem* it) const {
    DwarfVar v;
    const VariableDef d = it->data(ColName, Qt::UserRole).value<VariableDef>();
    v.name = d.name.isEmpty() ? itemPath(it) : d.name;
    v.addr = d.addr; v.size = d.size; v.type = d.type;
    v.bitOffset = d.bitOffset; v.bitWidth = d.bitWidth;
    v.isStruct = it->childCount() > 0;
    for (int i = 0; i < it->childCount(); ++i)
        v.members.append(toDwarf(it->child(i)));
    return v;
}

void VariableDialog::setSubtreeChecked(QTreeWidgetItem* it, Qt::CheckState st) {
    it->setCheckState(ColName, st);
    for (int i = 0; i < it->childCount(); ++i)
        setSubtreeChecked(it->child(i), st);
}

/* 全路径: 优先用节点数据里存的完整名, 回退到显示名拼接 */
static QString nodePath(QTreeWidgetItem* it) {
    const VariableDef d = it->data(ColName, Qt::UserRole).value<VariableDef>();
    if (!d.name.isEmpty()) return d.name;
    QStringList parts;
    for (QTreeWidgetItem* p = it; p && p->parent(); p = p->parent())
        parts.prepend(p->text(ColName));
    return parts.join('.');
}

void VariableDialog::onItemChanged(QTreeWidgetItem* item, int col) {
    /* 把子树中已勾「绘制」的叶子发出来(在监控添加后同步绘制列表) */
    auto leafDef = [](QTreeWidgetItem* it) -> VariableDef {
        return it->data(ColName, Qt::UserRole).value<VariableDef>();
    };
    std::function<void(QTreeWidgetItem*)> emitDrawnLeaves =
        [this, &emitDrawnLeaves, &leafDef](QTreeWidgetItem* it) {
        if (it->childCount() == 0) {
            if (it->checkState(ColDraw) == Qt::Checked)
                emit displayToggled(leafDef(it), true);
            return;
        }
        for (int i = 0; i < it->childCount(); ++i)
            emitDrawnLeaves(it->child(i));
    };

    if (col == ColName) {
        const bool checked = item->checkState(ColName) == Qt::Checked;
        if (item->childCount() > 0) {
            tree_->blockSignals(true);
            setSubtreeChecked(item, checked ? Qt::Checked : Qt::Unchecked);
            tree_->blockSignals(false);
            if (checked) {
                emit monitorAdded(toDwarf(item));   /* 整块监控 */
                emitDrawnLeaves(item);              /* 已勾绘制的成员同步进绘制列表 */
            } else {
                emit monitorRemoved(nodePath(item));
            }
        } else {
            if (checked) {
                emit monitorAdded(toDwarf(item));
                if (item->checkState(ColDraw) == Qt::Checked)
                    emit displayToggled(leafDef(item), true);
            } else {
                emit monitorRemoved(nodePath(item));
            }
        }
    } else if (col == ColDraw && item->childCount() == 0) {
        /* 仅影响绘制列表, 与监控互不相关 */
        emit displayToggled(leafDef(item),
                            item->checkState(ColDraw) == Qt::Checked);
    }
}

void VariableDialog::onImportElf() {
    const QString path = QFileDialog::getOpenFileName(
        this, "选择 ELF 文件", {}, "ELF 文件 (*.elf *.axf *.out *.o);;所有文件 (*)");
    if (path.isEmpty()) return;
    importElfFile(path);
}

/* 导入 ELF(DWARF 优先, symtab 兜底); interactive=false 供启动恢复静默跳过 */
bool VariableDialog::importElfFile(const QString& path, bool interactive) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray elf = f.readAll();
    f.close();

    auto* root = ensureFileRoot(QFileInfo(path).fileName());
    root->setData(ColName, Qt::UserRole + 3, path);             /* 完整路径(持久化) */
    root->setData(ColName, Qt::UserRole + 4, QStringLiteral("elf"));
    tree_->blockSignals(true);
    QVector<DwarfVar> dvars;
    if (DwarfParser::parse(elf, dvars) && !dvars.isEmpty()) {
        /* 源文件分组: ELF 根 → 源文件(含路径) → 变量 */
        QHash<QString, QTreeWidgetItem*> fileNodes;
        auto fileNode = [&](const QString& f) -> QTreeWidgetItem* {
            const QString key = f.isEmpty() ? "(未知)" : f;   /* 完整路径作唯一分组 key */
            if (QTreeWidgetItem* e = fileNodes.value(key, nullptr)) return e;
            auto* n = new QTreeWidgetItem(root);
            n->setText(ColName, key);                          /* 整行显示相对路径 */
            n->setToolTip(ColName, key);                       /* 悬停看完整路径 */
            n->setData(ColName, Qt::UserRole + 1, 1);          /* 分组标记(排序用) */
            n->setData(ColName, Qt::UserRole + 2, true);       /* 分组标记(delegate 用) */
            n->setFirstColumnSpanned(true);                    /* 跨整行, 其他列留空 */
            n->setForeground(ColName, QColor(0x0B, 0x57, 0xD0));   /* 文件路径深蓝色 */
            n->setSizeHint(0, QSize(0, n->sizeHint(0).height() + 4));   /* 行高 +4px */
            fileNodes.insert(key, n);
            return n;
        };
        std::function<void(QTreeWidgetItem*, const DwarfVar&)> addNode =
            [&](QTreeWidgetItem* parent, const DwarfVar& v) {
            VariableDef d;
            d.name = v.name; d.addr = v.addr; d.size = v.size;
            d.type = v.type; d.bitOffset = v.bitOffset; d.bitWidth = v.bitWidth;
            auto* it = new QTreeWidgetItem(parent);
            it->setText(ColName, shortVarName(v.name));
            it->setData(ColName, Qt::UserRole, QVariant::fromValue(d));
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(ColName, Qt::Unchecked);
            it->setText(ColAddr, "0x" + QString::number(v.addr, 16).rightJustified(8, '0').toUpper());  /* 基地址 */
            if (v.isArray) {
                const bool isPage = v.name.contains("..");   /* 分页分组节点 */
                if (isPage) {
                    it->setText(ColAddr, QString());         /* 分组不显示地址 */
                    it->setText(ColType, QString());         /* 分组不显示类型 */
                } else {
                    it->setText(ColSize, QString::number(v.size));   /* 数组总大小 */
                    it->setText(ColType, v.typeText.isEmpty() ? "数组" : v.typeText);
                }
                for (const auto& m : v.members) addNode(it, m);   /* 数组元素 */
            } else if (v.isStruct) {
                it->setText(ColSize, QString::number(v.size));   /* 结构体总大小 */
                it->setText(ColType, "struct");
                for (const auto& m : v.members) addNode(it, m);   /* 成员按定义顺序 */
            } else {
                it->setCheckState(ColDraw, Qt::Unchecked);
                it->setText(ColSize, QString::number(v.size));
                it->setText(ColType, v.bitOffset >= 0
                    ? QString("%1:%2").arg(typeName(v.type)).arg(v.bitWidth)
                    : (v.typeText.isEmpty() ? QString::fromLatin1(typeName(v.type)) : v.typeText));
            }
        };
        for (const auto& v : dvars) addNode(fileNode(v.file), v);
    } else {
                QVector<ElfSymbol> syms;
                QString err;
                if (!ElfParser::parse(path, syms, &err)) {
                    if (interactive) QMessageBox::warning(this, "导入失败", err);
                    tree_->blockSignals(false);
                    return false;
                }
                /* symtab 兜底无源文件信息, 归入 "(未知)" 分组 */
                auto* unk = new QTreeWidgetItem(root);
                unk->setText(ColName, "(未知)");
                unk->setData(ColName, Qt::UserRole + 1, 1);
                unk->setData(ColName, Qt::UserRole + 2, true);
                unk->setFirstColumnSpanned(true);
                unk->setForeground(ColName, QColor(0x0B, 0x57, 0xD0));   /* 文件路径深蓝色 */
                unk->setSizeHint(0, QSize(0, unk->sizeHint(0).height() + 4));   /* 行高 +4px */
                for (const auto& s : syms) {
                    VariableDef v;
                    v.name = s.name; v.addr = s.addr; v.size = s.size;
                    switch (s.size) {
                    case 1: v.type = VariableDef::Uint8;  break;
                    case 2: v.type = VariableDef::Uint16; break;
                    default: v.type = VariableDef::Uint32; break;
                    }
                    addLeafItem(unk, v);
                }
            }
            tree_->blockSignals(false);
        /* 用户没点过排序时, 默认名称列升序; 点过则沿用用户选择。
         * 排序规则不变: 顶层按列排序, 结构体成员保持定义顺序 */
        QHeaderView* hdr = tree_->header();
        const int sec = userSorted_ ? hdr->sortIndicatorSection() : int(ColName);
        const auto ord = userSorted_ ? hdr->sortIndicatorOrder() : Qt::AscendingOrder;
        if (!userSorted_) {
            tree_->blockSignals(true);
            hdr->setSortIndicator(sec, ord);   /* 只更新指示图标, 不触发重排 */
            tree_->blockSignals(false);
        }
        sortTopLevel(sec, ord);
        tree_->collapseAll();
        root->setExpanded(true);
        /* 展开源文件分组节点, 变量(结构体)节点保持折叠 */
        for (int i = 0; i < root->childCount(); ++i) {
            auto* g = root->child(i);
            if (g->data(ColName, Qt::UserRole + 1).toInt() == 1)
                g->setExpanded(true);
        }
    return true;
}

void VariableDialog::onImportSvd() {
    const QString path = QFileDialog::getOpenFileName(
        this, "选择 SVD 文件", {}, "SVD 文件 (*.svd *.xml);;所有文件 (*)");
    if (path.isEmpty()) return;
    importSvdFile(path);
}

/* 导入 SVD 寄存器文件; interactive=false 供启动恢复静默跳过 */
bool VariableDialog::importSvdFile(const QString& path, bool interactive) {
    if (!QFile::exists(path)) return false;
    QVector<SvdPeripheral> peris;
    QString err;
    if (!SvdParser::parse(path, peris, &err)) {
        if (interactive) QMessageBox::warning(this, "导入失败", err);
        return false;
    }
    auto* root = ensureFileRoot(QFileInfo(path).fileName());
    root->setData(ColName, Qt::UserRole + 3, path);             /* 完整路径(持久化) */
    root->setData(ColName, Qt::UserRole + 4, QStringLiteral("svd"));
    tree_->blockSignals(true);
    for (const auto& peri : peris) {
        auto* pIt = new QTreeWidgetItem(root);
        pIt->setText(ColName, peri.name);
        VariableDef pd;
        pd.name = peri.name; pd.addr = peri.baseAddr; pd.size = 4;
        pIt->setData(ColName, Qt::UserRole, QVariant::fromValue(pd));
        pIt->setFlags(pIt->flags() | Qt::ItemIsUserCheckable);
        pIt->setCheckState(ColName, Qt::Unchecked);
        pIt->setText(ColAddr, QStringLiteral("0x%1").arg(peri.baseAddr, 8, 16, QChar('0')));  /* 基地址 */
        pIt->setText(ColType, "外设");
        for (const auto& reg : peri.regs) {
            VariableDef rv;
            rv.name = peri.name + "." + reg.name;
            rv.addr = reg.addr; rv.size = reg.size / 8;
            rv.type = VariableDef::Uint32;
            if (reg.fields.isEmpty()) {
                addLeafItem(pIt, rv);   /* 寄存器按地址 */
            } else {
                auto* rIt = new QTreeWidgetItem(pIt);
                rIt->setText(ColName, reg.name);
                rIt->setData(ColName, Qt::UserRole, QVariant::fromValue(rv));
                rIt->setFlags(rIt->flags() | Qt::ItemIsUserCheckable);
                rIt->setCheckState(ColName, Qt::Unchecked);
                rIt->setText(ColAddr, "0x" + QString::number(rv.addr, 16).rightJustified(8, '0').toUpper());  /* 基地址 */
                rIt->setText(ColType, "寄存器");
                for (const auto& fld : reg.fields) {
                    VariableDef f = rv;
                    f.name = rv.name + "." + fld.name;
                    f.bitOffset = fld.bitOffset;
                    f.bitWidth = fld.bitWidth;
                    addLeafItem(rIt, f);   /* 位域按地址(原始顺序) */
                }
            }
        }
    }
    tree_->blockSignals(false);
    onSortByColumn(tree_->header()->sortIndicatorSection(),
                   tree_->header()->sortIndicatorOrder());
    tree_->collapseAll();
    root->setExpanded(true);
    return true;
}

/* ---------------- 持久化 ---------------- */
void VariableDialog::saveState(QSettings& s) const {
    /* 导入的文件(完整路径), 按类型分存 */
    QStringList elfFiles, svdFiles;
    for (int t = 0; t < tree_->topLevelItemCount(); ++t) {
        auto* r = tree_->topLevelItem(t);
        const QString path = r->data(ColName, Qt::UserRole + 3).toString();
        if (path.isEmpty()) continue;
        (r->data(ColName, Qt::UserRole + 4).toString() == "svd" ? svdFiles : elfFiles) << path;
    }
    s.setValue("vars/elfFiles", elfFiles);
    s.setValue("vars/svdFiles", svdFiles);

    /* 勾选状态: 只记最顶层被勾的节点(父勾选即覆盖子), 恢复时重放勾选动作 */
    QStringList mon, draw;
    std::function<void(QTreeWidgetItem*, bool)> walk =
        [&](QTreeWidgetItem* it, bool parentChecked) {
        const bool checked = it->checkState(ColName) == Qt::Checked;
        if (checked && !parentChecked) mon << nodePath(it);
        if (it->childCount() == 0 && it->checkState(ColDraw) == Qt::Checked)
            draw << nodePath(it);
        for (int i = 0; i < it->childCount(); ++i)
            walk(it->child(i), parentChecked || checked);
    };
    for (int t = 0; t < tree_->topLevelItemCount(); ++t)
        walk(tree_->topLevelItem(t), false);
    s.setValue("vars/monitored", mon);
    s.setValue("vars/drawn", draw);
}

void VariableDialog::restoreState(QSettings& s) {
    /* 重导入上次会话的文件(静默: 文件不存在/解析失败则跳过) */
    for (const QString& p : s.value("vars/elfFiles").toStringList())
        importElfFile(p, false);
    for (const QString& p : s.value("vars/svdFiles").toStringList())
        importSvdFile(p, false);
    if (tree_->topLevelItemCount() == 0) return;

    /* 全路径 -> 节点索引 */
    QHash<QString, QTreeWidgetItem*> byPath;
    std::function<void(QTreeWidgetItem*)> index =
        [&](QTreeWidgetItem* it) {
        byPath.insert(nodePath(it), it);
        for (int i = 0; i < it->childCount(); ++i) index(it->child(i));
    };
    for (int t = 0; t < tree_->topLevelItemCount(); ++t)
        index(tree_->topLevelItem(t));

    /* 重放勾选(不锁信号: 与手动勾选走完全相同的路径) */
    for (const QString& p : s.value("vars/monitored").toStringList())
        if (auto* it = byPath.value(p, nullptr))
            it->setCheckState(ColName, Qt::Checked);
    for (const QString& p : s.value("vars/drawn").toStringList())
        if (auto* it = byPath.value(p, nullptr))
            it->setCheckState(ColDraw, Qt::Checked);
}

void VariableDialog::onRemoveSelected() {
    const auto sel = tree_->selectedItems();
    if (sel.isEmpty()) return;
    for (auto* it : sel) delete it;
}

void VariableDialog::onSearch(const QString& text) {
    const QString q = text.trimmed();
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* it) {
        const bool match = q.isEmpty() || it->text(ColName).contains(q, Qt::CaseInsensitive);
        bool anyChildVisible = false;
        for (int i = 0; i < it->childCount(); ++i) {
            walk(it->child(i));
            anyChildVisible |= !it->child(i)->isHidden();
        }
        it->setHidden(!match && !anyChildVisible);
        if (match || anyChildVisible) it->setExpanded(true);
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        walk(tree_->topLevelItem(i));
}
