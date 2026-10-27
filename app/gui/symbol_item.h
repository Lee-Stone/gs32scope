#ifndef SYMBOL_ITEM_H
#define SYMBOL_ITEM_H
/*
 * SymbolItem - 符号树的节点条目
 * 四列: 变量名 | 地址 | 大小(字节) | 类型
 * 排序: 名字列按 cmpVarName 规则; 地址/大小列按数值。
 */
#include "core/variable_def.h"
#include <QTreeWidgetItem>

class SymbolItem : public QTreeWidgetItem
{
public:
    enum { ColName = 0, ColAddr = 1, ColSize = 2, ColType = 3 };
    static constexpr int RoleDef = Qt::UserRole;      /* VariableDef */
    static constexpr int RoleLeaf = Qt::UserRole + 1; /* bool: 是否可采样叶子 */

    using QTreeWidgetItem::QTreeWidgetItem;

    void setDef(const VariableDef& v) { setData(ColName, RoleDef, QVariant::fromValue(v)); }
    VariableDef def() const { return data(ColName, RoleDef).value<VariableDef>(); }
    void setLeaf(bool leaf) { setData(ColName, RoleLeaf, leaf); }
    bool isLeaf() const { return data(ColName, RoleLeaf).toBool(); }

    void refreshRow() {
        setText(ColAddr, QStringLiteral("0x%1").arg(def().addr, 8, 16, QChar('0')));
        if (isLeaf()) {
            setText(ColSize, QString::number(def().size));
            QString t = QString::fromLatin1(typeName(def().type));
            if (def().bitOffset >= 0)
                t += QStringLiteral(":%1").arg(def().bitWidth);
            setText(ColType, t);
        } else {
            setText(ColSize, QString());
            setText(ColType, QString());
        }
    }

protected:
    bool operator<(const QTreeWidgetItem& other) const override {
        const QTreeWidget* tw = treeWidget();
        const int col = tw ? tw->sortColumn() : ColName;
        const SymbolItem* o = static_cast<const SymbolItem*>(&other);
        if (col == ColAddr) return def().addr < o->def().addr;
        if (col == ColSize) return def().size < o->def().size;
        return cmpVarName(text(ColName), other.text(ColName)) < 0;
    }
};

#endif /* SYMBOL_ITEM_H */
