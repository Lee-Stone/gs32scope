#include "monitor_model.h"
#include <cmath>
#include <functional>

namespace {
const QColor kColors[] = {
    QColor(0x2F, 0xD8, 0x58), QColor(0x3F, 0xB9, 0xFF),
    QColor(0xFF, 0xB4, 0x2E), QColor(0xFF, 0x5C, 0x7A),
    QColor(0xC8, 0x8A, 0xFF), QColor(0x2E, 0xE6, 0xD6),
    QColor(0xFF, 0x8A, 0x3D), QColor(0x8F, 0xD4, 0xFF),
    QColor(0xD8, 0x6B, 0xFF), QColor(0x7E, 0xE0, 0x8B),
    QColor(0xFF, 0xD3, 0x5C), QColor(0xFF, 0x7A, 0xB8),
};
constexpr int kPaletteSize = int(sizeof(kColors) / sizeof(kColors[0]));
}

MonitorModel::MonitorModel(QObject* parent) : QObject(parent) {}
MonitorModel::~MonitorModel() {
    /* 析构期间不能发 changed(): 监控树/绘制列表可能正在被销毁,
     * rebuild() 访问半析构对象会在关闭时崩溃. */
    blockSignals(true);
    clear();
}

QColor MonitorModel::nextColor() {
    /* 前 12 个用高对比调色板; 超过后按黄金角生成新色相,
     * 并跳过已用颜色, 保证连续添加变量时颜色不重复。 */
    for (int attempt = 0; attempt < 4096; ++attempt) {
        const int idx = colorIdx_++;
        QColor c;
        if (idx < kPaletteSize) {
            c = kColors[idx];
        } else {
            const int n = idx - kPaletteSize;
            const int hue = int(std::fmod(n * 137.508, 360.0));
            const int sat = 205 + (n % 3) * 18;
            const int val = 238 - (n % 2) * 22;
            c = QColor::fromHsv(hue, sat, val);
        }
        if (c.isValid() && !usedColors_.contains(c.rgba())) {
            usedColors_.insert(c.rgba());
            return c;
        }
    }
    return QColor(0x3F, 0xB9, 0xFF);
}

int MonitorModel::addFlatLeaf(const QString& path, const VariableDef& d, int rootIdx) {
    MonitorLeaf lf;
    lf.path = path;
    lf.def = d;
    lf.rootIdx = rootIdx;
    lf.color = nextColor();
    leaves_.append(lf);
    const int idx = leaves_.size() - 1;
    leafIdx_.insert(path, idx);
    roots_.at(rootIdx)->leaves.append(idx);
    return idx;
}

/* 由 DwarfVar 构建显示树节点, 叶子同时入扁平表 */
MonitorNode* MonitorModel::buildNode(const DwarfVar& v, const QString& prefix,
                                     QVector<int>& flatLeaves, int rootIdx) {
    auto* node = new MonitorNode();
    node->name = shortVarName(v.name);
    node->path = prefix.isEmpty() ? v.name : prefix + "." + v.name;
    node->def.name = node->path;
    node->def.addr = v.addr;
    node->def.size = v.size;
    node->def.type = v.type;
    node->def.bitOffset = v.bitOffset;
    node->def.bitWidth = v.bitWidth;
    node->isStruct = v.isStruct;
    if (v.isStruct) {
        for (const auto& m : v.members)
            node->children.append(buildNode(m, node->path, flatLeaves, rootIdx));
    } else {
        node->leafIdx = addFlatLeaf(node->path, node->def, rootIdx);
        flatLeaves.append(node->leafIdx);
    }
    return node;
}

void MonitorModel::addLeafRoot(const VariableDef& v) {
    auto* r = new MonitorRoot();
    r->name = v.name;
    r->typeText = QString::fromLatin1(typeName(v.type));
    r->addr = v.addr; r->size = v.size;
    r->isStruct = false;
    r->color = nextColor();
    roots_.append(r);
    r->tree = new MonitorNode();
    r->tree->name = shortVarName(v.name);
    r->tree->path = v.name;
    r->tree->def = v;
    r->tree->isStruct = false;
    r->tree->leafIdx = addFlatLeaf(v.name, v, roots_.size() - 1);
    emit changed();
}

void MonitorModel::addDwarfRoot(const DwarfVar& v) {
    auto* r = new MonitorRoot();
    r->name = v.name;
    r->addr = v.addr; r->size = v.size;
    r->isStruct = v.isStruct;
    r->typeText = v.isStruct ? "struct" : QString::fromLatin1(typeName(v.type));
    r->color = nextColor();
    roots_.append(r);
    QVector<int> flat;
    r->tree = buildNode(v, QString(), flat, roots_.size() - 1);
    emit changed();
}

int MonitorModel::addDrawOnlyRoot(const VariableDef& v) {
    auto* r = new MonitorRoot();
    r->name = v.name;
    r->typeText = QString::fromLatin1(typeName(v.type));
    r->addr = v.addr; r->size = v.size;
    r->isStruct = false;
    r->color = nextColor();
    roots_.append(r);
    r->tree = new MonitorNode();
    r->tree->name = shortVarName(v.name);
    r->tree->path = v.name;
    r->tree->def = v;
    r->tree->isStruct = false;
    const int li = addFlatLeaf(v.name, v, roots_.size() - 1);
    leaves_[li].monitored = false;
    leaves_[li].shown = true;   /* 绘制专用: 默认进绘制列表 */
    leaves_[li].shownSeq = ++shownSeq_;
    r->tree->leafIdx = li;
    emit changed();
    return li;
}

void MonitorModel::setLeafMonitored(int leafIdx, bool m) {
    if (leafIdx < 0 || leafIdx >= leaves_.size()) return;
    leaves_[leafIdx].monitored = m;
    emit changed();   /* 监控树需刷新(显示/隐藏该行) */
}

void MonitorModel::removeRoot(int rootIdx) {
    if (rootIdx < 0 || rootIdx >= roots_.size()) return;
    for (int li : roots_.at(rootIdx)->leaves)
        leafIdx_.remove(leaves_.at(li).path);
    delete roots_.at(rootIdx);
    roots_.removeAt(rootIdx);
    /* 重建叶子表: 按剩余根的顺序收集 */
    QVector<MonitorLeaf> keep;
    QHash<QString, int>  newIdx;
    for (int ri = 0; ri < roots_.size(); ++ri) {
        MonitorRoot* r = roots_[ri];
        QVector<int> newList;
        for (int oldLi : r->leaves) {
            MonitorLeaf l = leaves_.at(oldLi);
            l.rootIdx = ri;
            const int ni = keep.size();
            keep.append(l);
            newList.append(ni);
            newIdx.insert(l.path, ni);
        }
        r->leaves = newList;
    }
    leaves_ = keep;
    leafIdx_ = newIdx;
    /* 同步节点树里的 leafIdx(removeRoot 后扁平下标已重排) */
    std::function<void(MonitorNode*)> remap = [&](MonitorNode* n) {
        if (!n->isStruct && n->leafIdx >= 0)
            n->leafIdx = leafIdx_.value(n->path, -1);
        for (MonitorNode* c : n->children) remap(c);
    };
    for (MonitorRoot* r : roots_) if (r->tree) remap(r->tree);
    emit changed();
}

void MonitorModel::clear() {
    for (MonitorRoot* r : roots_) delete r;
    roots_.clear();
    leaves_.clear(); leafIdx_.clear(); usedColors_.clear();
    colorIdx_ = 0; shownSeq_ = 0;
    emit changed();
}

void MonitorModel::updateLeafValue(int leafIdx, double v) {
    updateLeafValues(leafIdx, QVector<double>{v});
}

void MonitorModel::updateLeafValues(int leafIdx, const QVector<double>& values) {
    if (leafIdx < 0 || leafIdx >= leaves_.size() || values.isEmpty()) return;
    MonitorLeaf& l = leaves_[leafIdx];
    for (double v : values) {
        l.cur = v;
        if (l.n == 0) { l.min = l.max = v; }
        else { if (v < l.min) l.min = v; if (v > l.max) l.max = v; }
        ++l.n;
    }
    emit leafValueChanged(leafIdx);
}

void MonitorModel::setLeafCurrent(int leafIdx, double v) {
    if (leafIdx < 0 || leafIdx >= leaves_.size()) return;
    leaves_[leafIdx].cur = v;
    emit leafValueChanged(leafIdx);
}

void MonitorModel::setLeafHex(int leafIdx, bool hex) {
    if (leafIdx < 0 || leafIdx >= leaves_.size()) return;
    if (leaves_[leafIdx].hex == hex) return;
    leaves_[leafIdx].hex = hex;
    emit leafValueChanged(leafIdx);
}

void MonitorModel::setLeafColor(int leafIdx, const QColor& color) {
    if (leafIdx < 0 || leafIdx >= leaves_.size() || !color.isValid()) return;
    if (leaves_[leafIdx].color == color) return;
    leaves_[leafIdx].color = color;
    usedColors_.insert(color.rgba());
    emit leafColorChanged(leafIdx);
}

void MonitorModel::setLeafShown(int leafIdx, bool on) {
    if (leafIdx < 0 || leafIdx >= leaves_.size()) return;
    if (leaves_[leafIdx].shown == on) return;
    leaves_[leafIdx].shown = on;
    leaves_[leafIdx].shownSeq = on ? ++shownSeq_ : 0;
    emit leafShownChanged(leafIdx);
}

void MonitorModel::setLeavesShown(const QVector<int>& leafIdxs, bool on) {
    bool any = false;
    for (int li : leafIdxs) {
        if (li < 0 || li >= leaves_.size()) continue;
        if (leaves_[li].shown != on) {
            leaves_[li].shown = on;
            leaves_[li].shownSeq = on ? ++shownSeq_ : 0;
            any = true;
        }
    }
    if (any) emit leavesShownChanged();
}

void MonitorModel::resetStats() {
    for (auto& l : leaves_) { l.n = 0; l.cur = l.min = l.max = 0; }
    emit changed();
}
