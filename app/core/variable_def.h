#ifndef VARIABLE_DEF_H
#define VARIABLE_DEF_H
/*
 * 监控变量定义。ELF(symtab/DWARF) 与 SVD 导入后统一落到此结构。
 * 复合类型(结构体)在 UI 层表现为树的父节点, 叶子节点才是采样单元。
 * 位域成员: bitOffset >= 0 时, 引擎解码后做 (v >> bitOffset) & mask。
 */
#include <QString>
#include <QMetaType>
#include <QtGlobal>
#include <cstdint>

struct VariableDef {
    enum Type { Float, Int8, Int16, Int32, Uint8, Uint16, Uint32 };
    QString  name;        /* 全路径名: "sim.vehicle.speed" / "USART1.SR.TXE" */
    uint32_t addr = 0;    /* 读取地址 */
    uint32_t size = 4;    /* 读取字节数 */
    Type     type = Float;
    int      bitOffset = -1;   /* -1 = 非位域 */
    int      bitWidth  = 0;

    uint32_t mask() const {
        if (bitOffset < 0) return 0xFFFFFFFFu;
        return bitWidth >= 32 ? 0xFFFFFFFFu
                              : ((1u << bitWidth) - 1u) << bitOffset;
    }
};
Q_DECLARE_METATYPE(VariableDef)

/* 显示短名: 去掉点路径前缀, 但保留数组下标(如 "obj.a[99..0]" -> "a[99..0]",
 * 方括号内的 ".." 不当作路径分隔符) */
inline QString shortVarName(const QString& full) {
    int p = -1, depth = 0;
    for (int i = full.size() - 1; i >= 0; --i) {
        const QChar c = full[i];
        if (c == ']') ++depth;
        else if (c == '[') --depth;
        else if (c == '.' && depth == 0) { p = i; break; }
    }
    return p < 0 ? full : full.mid(p + 1);
}

/* 类型显示名 */
inline const char* typeName(VariableDef::Type t) {
    switch (t) {
    case VariableDef::Float:  return "float";
    case VariableDef::Int8:   return "int8";
    case VariableDef::Int16:  return "int16";
    case VariableDef::Int32:  return "int32";
    case VariableDef::Uint8:  return "uint8";
    case VariableDef::Uint16: return "uint16";
    case VariableDef::Uint32: return "uint32";
    }
    return "?";
}

/* 变量名排序: 不区分大小写按字母升序; 同字母大小写相遇时小写在前 */
inline int cmpVarName(const QString& a, const QString& b) {
    const int n = qMin(a.size(), b.size());
    for (int i = 0; i < n; ++i) {
        const QChar ca = a[i], cb = b[i];
        const QChar la = ca.toLower(), lb = cb.toLower();
        if (la != lb) return la < lb ? -1 : 1;
        if (ca != cb) {           /* 同字母不同大小写: 小写在前 */
            if (ca.isLower() != cb.isLower()) return ca.isLower() ? -1 : 1;
        }
    }
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return 0;
}

#endif /* VARIABLE_DEF_H */
