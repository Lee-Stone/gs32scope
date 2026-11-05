#include "variable_dialog.h"
#include "symbols/elf_parser.h"
#include "symbols/svd_parser.h"
#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QHeaderView>
#include <QSignalBlocker>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>
#include <functional>

namespace {   /* 与 VariableDialog 私有枚举一致 */
enum { ColName = 0, ColAddr = 1, ColSize = 2, ColType = 3 };

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

} // namespace

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
    tree_->setColumnCount(4);
    tree_->setHeaderLabels({"变量", "地址", "大小", "类型"});
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
    tree_->header()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    tree_->header()->setStretchLastSection(false);
    /* 地址/大小列内容居中 */
    tree_->setItemDelegateForColumn(ColAddr, new AlignCenterDelegate(tree_));
    tree_->setItemDelegateForColumn(ColSize, new AlignCenterDelegate(tree_));
    /* 内建排序会递归重排/翻转结构体成员, 禁用;
     * 改为表头点击只排顶层(文件根下), 成员保持定义顺序(地址从小到大) */
    tree_->setSortingEnabled(false);
    tree_->header()->setSectionsClickable(true);
    tree_->header()->setSortIndicatorShown(true);
    connect(tree_->header(), &QHeaderView::sortIndicatorChanged,
            this, &VariableDialog::onSortByColumn);
    connect(tree_, &QTreeWidget::itemChanged, this, &VariableDialog::onItemChanged);
    /* 右键只负责监控开关; 波形选择统一在监控表颜色列完成 */
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        auto* it = tree_->itemAt(pos);
        if (!it || !(it->flags() & Qt::ItemIsUserCheckable)) return;
        QMenu menu(this);
        const bool mon = it->checkState(ColName) == Qt::Checked;
        menu.addAction(mon ? "取消监控" : "监控", this, [it, mon] {
            it->setCheckState(ColName, mon ? Qt::Unchecked : Qt::Checked);
        });
        menu.exec(tree_->viewport()->mapToGlobal(pos));
    });
    tree_->setItemDelegate(new FileGroupDelegate(tree_));   /* 源文件分组整行绘制 */
    lay->addWidget(tree_, 1);

    auto* row = new QHBoxLayout();
    auto* add = new QPushButton("添加…", this);
    add->setFixedSize(100, 30);
    auto* menu = new QMenu(add);
    menu->addAction(QStringLiteral("从 ELF 导入变量…"), this, &VariableDialog::onImportElf);
    menu->addAction(QStringLiteral("从 SVD 导入寄存器…"), this, &VariableDialog::onImportSvd);
    add->setMenu(menu);
    row->addWidget(add);
    row->addStretch(1);
    lay->addLayout(row);

    /* 重新编译后检测导入的 ELF 是否变化; 用户可选择更新/自动更新 */
    autoReloadElf_ = QSettings().value("vars/elfAutoReload", false).toBool();
    elfTimer_.setInterval(1500);
    connect(&elfTimer_, &QTimer::timeout, this, &VariableDialog::checkElfChanged);
    elfTimer_.start();
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
    it->setText(ColAddr, "0x" + QString::number(v.addr, 16).rightJustified(8, '0').toUpper());
    it->setText(ColSize, QString::number(v.size));
    it->setText(ColType, v.bitOffset >= 0
        ? QString("%1:%2").arg(typeName(v.type)).arg(v.bitWidth)
        : QString::fromLatin1(typeName(v.type)));
    it->setTextAlignment(ColName, Qt::AlignCenter);
    it->setTextAlignment(ColAddr, Qt::AlignCenter);
    it->setTextAlignment(ColSize, Qt::AlignCenter);
    it->setTextAlignment(ColType, Qt::AlignCenter);
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

void VariableDialog::uncheckMonitored(const QString& path) {
    if (!tree_ || path.isEmpty()) return;
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> find =
        [&](QTreeWidgetItem* it) -> QTreeWidgetItem* {
        if (nodePath(it) == path) return it;
        for (int i = 0; i < it->childCount(); ++i)
            if (auto* hit = find(it->child(i))) return hit;
        return nullptr;
    };
    QTreeWidgetItem* target = nullptr;
    for (int t = 0; t < tree_->topLevelItemCount() && !target; ++t)
        target = find(tree_->topLevelItem(t));
    if (!target) return;
    /* 只同步 UI, 不再发 monitorRemoved(模型已经从监控表移除) */
    QSignalBlocker block(tree_);
    setSubtreeChecked(target, Qt::Unchecked);
}

void VariableDialog::clearMonitoredChecks() {
    if (!tree_) return;
    QSignalBlocker block(tree_);
    std::function<void(QTreeWidgetItem*)> clear = [&](QTreeWidgetItem* it) {
        it->setCheckState(ColName, Qt::Unchecked);
        for (int i = 0; i < it->childCount(); ++i) clear(it->child(i));
    };
    for (int t = 0; t < tree_->topLevelItemCount(); ++t)
        clear(tree_->topLevelItem(t));
}

void VariableDialog::onItemChanged(QTreeWidgetItem* item, int col) {
    if (col != ColName || !item) return;
    const bool checked = item->checkState(ColName) == Qt::Checked;
    if (item->childCount() > 0) {
        tree_->blockSignals(true);
        setSubtreeChecked(item, checked ? Qt::Checked : Qt::Unchecked);
        tree_->blockSignals(false);
        if (checked) emit monitorAdded(toDwarf(item));   /* 整块监控 */
        else emit monitorRemoved(nodePath(item));
    } else {
        if (checked) emit monitorAdded(toDwarf(item));
        else emit monitorRemoved(nodePath(item));
    }
}

void VariableDialog::onImportElf() {
    const QString path = QFileDialog::getOpenFileName(
        this, "选择 ELF 文件", {}, "ELF 文件 (*.elf *.axf *.out *.o);;所有文件 (*)");
    if (path.isEmpty()) return;
    importElfFile(path);
}

static bool isElfRoot(QTreeWidgetItem* it) {
    return it && it->data(0, Qt::UserRole + 4).toString() == QLatin1String("elf");
}

QStringList VariableDialog::collectElfCheckedPaths() const {
    QStringList out;
    std::function<void(QTreeWidgetItem*, bool)> walk =
        [&](QTreeWidgetItem* it, bool parentChecked) {
        const bool checked = it->checkState(ColName) == Qt::Checked;
        if (checked && !parentChecked) out << nodePath(it);
        for (int i = 0; i < it->childCount(); ++i)
            walk(it->child(i), parentChecked || checked);
    };
    for (int t = 0; t < tree_->topLevelItemCount(); ++t)
        if (isElfRoot(tree_->topLevelItem(t)))
            walk(tree_->topLevelItem(t), false);
    return out;
}

void VariableDialog::removeElfRoots(QStringList* checkedPaths) {
    QStringList paths;
    for (int t = tree_->topLevelItemCount() - 1; t >= 0; --t) {
        QTreeWidgetItem* root = tree_->topLevelItem(t);
        if (!isElfRoot(root)) continue;
        std::function<void(QTreeWidgetItem*, bool)> walk =
            [&](QTreeWidgetItem* it, bool parentChecked) {
            const bool checked = it->checkState(ColName) == Qt::Checked;
            if (checked && !parentChecked) paths << nodePath(it);
            for (int i = 0; i < it->childCount(); ++i)
                walk(it->child(i), parentChecked || checked);
        };
        walk(root, false);
        tree_->takeTopLevelItem(t);
        delete root;
    }
    /* 树节点已删除, 再通知模型移除旧根; 只发最上层勾选项 */
    for (const QString& p : paths) emit monitorRemoved(p);
    if (checkedPaths) *checkedPaths = paths;
}

void VariableDialog::recheckPaths(const QStringList& paths) {
    if (paths.isEmpty()) return;
    QHash<QString, QTreeWidgetItem*> byPath;
    std::function<void(QTreeWidgetItem*)> index = [&](QTreeWidgetItem* it) {
        byPath.insert(nodePath(it), it);
        for (int i = 0; i < it->childCount(); ++i) index(it->child(i));
    };
    for (int t = 0; t < tree_->topLevelItemCount(); ++t)
        index(tree_->topLevelItem(t));
    for (const QString& p : paths) {
        if (auto* it = byPath.value(p, nullptr)) {
            if (it->flags() & Qt::ItemIsUserCheckable)
                it->setCheckState(ColName, Qt::Checked);
        }
    }
}

void VariableDialog::updateElfSignature() {
    QFileInfo fi(elfPath_);
    if (fi.exists() && fi.isFile()) {
        elfMtime_ = fi.lastModified().toMSecsSinceEpoch();
        elfSize_ = fi.size();
    } else {
        elfMtime_ = 0;
        elfSize_ = -1;
    }
}

bool VariableDialog::reloadElfFile(const QString& path) {
    return importElfFile(path, false);
}

void VariableDialog::checkElfChanged() {
    if (elfPath_.isEmpty() || reloadingElf_ || elfPromptOpen_) return;
    QFileInfo fi(elfPath_);
    if (!fi.exists() || !fi.isFile()) return;
    const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
    const qint64 size = fi.size();
    if (mtime == elfMtime_ && size == elfSize_) return;
    /* 先更新签名, 避免用户选择"否"后每 1.5s 重复弹窗 */
    elfMtime_ = mtime;
    elfSize_ = size;

    if (autoReloadElf_) {
        reloadElfFile(elfPath_);
        return;
    }

    elfPromptOpen_ = true;
    QMessageBox box(this);
    box.setWindowTitle("ELF 已更新");
    box.setIcon(QMessageBox::Question);
    box.setText(QStringLiteral("检测到导入的 ELF 文件已变化：\n%1\n\n是否重新加载变量？")
                    .arg(elfPath_));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::Yes);
    if (auto* yes = box.button(QMessageBox::Yes)) yes->setText("是");
    if (auto* no = box.button(QMessageBox::No)) no->setText("否");
    auto* autoBox = new QCheckBox("不再提示，后续自动更新", &box);
    box.setCheckBox(autoBox);
    const int ret = box.exec();
    elfPromptOpen_ = false;
    if (autoBox->isChecked()) {
        autoReloadElf_ = true;
        QSettings().setValue("vars/elfAutoReload", true);
    }
    if (ret == QMessageBox::Yes)
        reloadElfFile(elfPath_);
}

/* 导入 ELF(DWARF 优先, symtab 兜底); 只保留一个 ELF,
 * 重新导入/重新编译时覆盖旧 ELF, 并保留同名变量的勾选状态。 */
bool VariableDialog::importElfFile(const QString& path, bool interactive) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (interactive) QMessageBox::warning(this, "导入失败", "无法读取: " + path);
        return false;
    }
    const QByteArray elf = f.readAll();
    f.close();

    /* 先完整解析, 解析失败时不动现有 ELF, 避免编译中间文件把变量表清空 */
    QVector<DwarfVar> dvars;
    QVector<ElfSymbol> syms;
    QString parseErr;
    const bool hasDwarf = DwarfParser::parse(elf, dvars) && !dvars.isEmpty();
    if (!hasDwarf) {
        if (!ElfParser::parse(path, syms, &parseErr)) {
            if (interactive) QMessageBox::warning(this, "导入失败", parseErr);
            return false;
        }
    }

    elfVars_ = hasDwarf ? dvars : QVector<DwarfVar>();
    emit elfReloadStarted();
    reloadingElf_ = true;
    QStringList recheck;
    removeElfRoots(&recheck);

    auto* root = ensureFileRoot(path);
    root->setText(ColName, path);                               /* 只显示 ELF 路径 */
    root->setToolTip(ColName, path);
    root->setData(ColName, Qt::UserRole + 3, path);             /* 完整路径(持久化) */
    root->setData(ColName, Qt::UserRole + 4, QStringLiteral("elf"));
    root->setFirstColumnSpanned(true);
    tree_->blockSignals(true);
    if (hasDwarf) {
        /* 源文件分组: ELF 根 → 源文件(含路径) → 变量 */
        QHash<QString, QTreeWidgetItem*> fileNodes;
        auto fileNode = [&](const QString& fname) -> QTreeWidgetItem* {
            const QString key = fname.isEmpty() ? "(未知)" : fname;
            if (QTreeWidgetItem* e = fileNodes.value(key, nullptr)) return e;
            auto* n = new QTreeWidgetItem(root);
            n->setText(ColName, key);
            n->setToolTip(ColName, key);
            n->setData(ColName, Qt::UserRole + 1, 1);
            n->setData(ColName, Qt::UserRole + 2, true);
            n->setFirstColumnSpanned(true);
            n->setForeground(ColName, QColor(0x0B, 0x57, 0xD0));
            n->setSizeHint(0, QSize(0, n->sizeHint(0).height() + 4));
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
            it->setText(ColAddr, "0x" + QString::number(v.addr, 16).rightJustified(8, '0').toUpper());
            if (v.isArray) {
                const bool isPage = v.name.contains("..");
                if (isPage) {
                    it->setText(ColAddr, QString());
                    it->setText(ColType, QString());
                } else {
                    it->setText(ColSize, QString::number(v.size));
                    it->setText(ColType, v.typeText.isEmpty() ? "数组" : v.typeText);
                }
                for (const auto& m : v.members) addNode(it, m);
            } else if (v.isStruct) {
                it->setText(ColSize, QString::number(v.size));
                it->setText(ColType, "struct");
                for (const auto& m : v.members) addNode(it, m);
            } else {
                it->setText(ColSize, QString::number(v.size));
                it->setText(ColType, v.bitOffset >= 0
                    ? QString("%1:%2").arg(typeName(v.type)).arg(v.bitWidth)
                    : (v.typeText.isEmpty() ? QString::fromLatin1(typeName(v.type)) : v.typeText));
            }
            it->setTextAlignment(ColName, Qt::AlignCenter);
            it->setTextAlignment(ColAddr, Qt::AlignCenter);
            it->setTextAlignment(ColSize, Qt::AlignCenter);
            it->setTextAlignment(ColType, Qt::AlignCenter);
        };
        for (const auto& v : dvars) addNode(fileNode(v.file), v);
    } else {
        auto* unk = new QTreeWidgetItem(root);
        unk->setText(ColName, "(未知)");
        unk->setData(ColName, Qt::UserRole + 1, 1);
        unk->setData(ColName, Qt::UserRole + 2, true);
        unk->setFirstColumnSpanned(true);
        unk->setForeground(ColName, QColor(0x0B, 0x57, 0xD0));
        unk->setSizeHint(0, QSize(0, unk->sizeHint(0).height() + 4));
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

    QHeaderView* hdr = tree_->header();
    const int sec = userSorted_ ? hdr->sortIndicatorSection() : int(ColName);
    const auto ord = userSorted_ ? hdr->sortIndicatorOrder() : Qt::AscendingOrder;
    if (!userSorted_) {
        tree_->blockSignals(true);
        hdr->setSortIndicator(sec, ord);
        tree_->blockSignals(false);
    }
    sortTopLevel(sec, ord);
    tree_->collapseAll();
    root->setExpanded(true);
    for (int i = 0; i < root->childCount(); ++i) {
        auto* g = root->child(i);
        if (g->data(ColName, Qt::UserRole + 1).toInt() == 1)
            g->setExpanded(true);
    }

    elfPath_ = path;
    updateElfSignature();
    reloadingElf_ = false;
    recheckPaths(recheck);
    emit elfReloadFinished();
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
        /* 外设整体监控时的读取跨度 = 最远寄存器末尾 - 基地址。
         * 旧实现固定 size=4, 子寄存器分布在偏移上会导致采样缓冲越界。 */
        uint32_t periSize = 4;
        for (const auto& reg : peri.regs) {
            if (reg.addr < peri.baseAddr) continue;
            const uint32_t off = reg.addr - peri.baseAddr;
            const uint32_t bytes = std::max<uint32_t>(reg.size / 8, 4);
            periSize = std::max<uint32_t>(periSize, off + bytes);
        }
        VariableDef pd;
        pd.name = peri.name; pd.addr = peri.baseAddr; pd.size = periSize;
        pIt->setData(ColName, Qt::UserRole, QVariant::fromValue(pd));
        pIt->setFlags(pIt->flags() | Qt::ItemIsUserCheckable);
        pIt->setCheckState(ColName, Qt::Unchecked);
        pIt->setText(ColAddr, QStringLiteral("0x%1").arg(peri.baseAddr, 8, 16, QChar('0')));  /* 基地址 */
        pIt->setText(ColType, "外设");
        pIt->setTextAlignment(ColName, Qt::AlignCenter);
        pIt->setTextAlignment(ColAddr, Qt::AlignCenter);
        pIt->setTextAlignment(ColType, Qt::AlignCenter);
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
                rIt->setTextAlignment(ColName, Qt::AlignCenter);
                rIt->setTextAlignment(ColAddr, Qt::AlignCenter);
                rIt->setTextAlignment(ColType, Qt::AlignCenter);
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
    s.setValue("vars/elfFile", elfFiles.isEmpty() ? QString() : elfFiles.first());
    s.setValue("vars/svdFiles", svdFiles);

    /* 勾选状态: 只记最顶层被勾的节点(父勾选即覆盖子), 恢复时重放勾选动作 */
    QStringList mon;
    std::function<void(QTreeWidgetItem*, bool)> walk =
        [&](QTreeWidgetItem* it, bool parentChecked) {
        const bool checked = it->checkState(ColName) == Qt::Checked;
        if (checked && !parentChecked) mon << nodePath(it);
        for (int i = 0; i < it->childCount(); ++i)
            walk(it->child(i), parentChecked || checked);
    };
    for (int t = 0; t < tree_->topLevelItemCount(); ++t)
        walk(tree_->topLevelItem(t), false);
    s.setValue("vars/monitored", mon);
}

void VariableDialog::restoreState(QSettings& s) {
    /* 只恢复一个 ELF(新格式 vars/elfFile; 旧格式列表取最后一个) */
    QString elfPath = s.value("vars/elfFile").toString();
    if (elfPath.isEmpty()) {
        const QStringList old = s.value("vars/elfFiles").toStringList();
        if (!old.isEmpty()) elfPath = old.last();
    }
    if (!elfPath.isEmpty()) importElfFile(elfPath, false);
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
}

void VariableDialog::onRemoveSelected() {
    const auto sel = tree_->selectedItems();
    if (sel.isEmpty()) return;

    /* 只处理最外层选中项(父子同时选中时删父即可), 先通知模型再删树项 */
    QSet<QTreeWidgetItem*> tops;
    for (auto* it : sel) {
        bool parentSelected = false;
        for (QTreeWidgetItem* p = it->parent(); p; p = p->parent())
            if (sel.contains(p)) { parentSelected = true; break; }
        if (!parentSelected) tops.insert(it);
    }

    std::function<void(QTreeWidgetItem*)> notify = [&](QTreeWidgetItem* it) {
        const VariableDef d = it->data(ColName, Qt::UserRole).value<VariableDef>();
        if (!d.name.isEmpty())
            emit monitorRemoved(d.name);            /* 整根移除(名称匹配的根) */
        for (int i = 0; i < it->childCount(); ++i) notify(it->child(i));
    };
    for (auto* it : tops) notify(it);

    /* 先摘除再 delete; 删除后清理空文件根 */
    for (auto* it : tops) {
        if (it->parent()) it->parent()->removeChild(it);
        else tree_->takeTopLevelItem(tree_->indexOfTopLevelItem(it));
        delete it;
    }
    for (int t = tree_->topLevelItemCount() - 1; t >= 0; --t)
        if (tree_->topLevelItem(t)->childCount() == 0)
            delete tree_->takeTopLevelItem(t);
}

bool VariableDialog::importElf(const QString& path) {
    return importElfFile(path, false);
}

bool VariableDialog::importSvd(const QString& path) {
    return importSvdFile(path, false);
}

void VariableDialog::monitorAll() {
    /* 文件根 -> 源文件分组/外设 -> 变量: 勾选所有顶层变量/外设。
     * 勾选父节点会由 onItemChanged 递归勾选子节点并入监控模型。 */
    for (int t = 0; t < tree_->topLevelItemCount(); ++t) {
        auto* fileRoot = tree_->topLevelItem(t);
        for (int i = 0; i < fileRoot->childCount(); ++i) {
            auto* c = fileRoot->child(i);
            if (c->data(ColName, Qt::UserRole + 1).toInt() == 1) {
                /* 源文件分组: 逐个变量 */
                for (int k = 0; k < c->childCount(); ++k) {
                    auto* v = c->child(k);
                    if (v->flags() & Qt::ItemIsUserCheckable)
                        v->setCheckState(ColName, Qt::Checked);
                }
            } else if (c->flags() & Qt::ItemIsUserCheckable) {
                c->setCheckState(ColName, Qt::Checked);
            }
        }
    }
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
