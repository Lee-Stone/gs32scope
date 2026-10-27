#ifndef DWARF_PARSER_H
#define DWARF_PARSER_H
/*
 * DwarfParser - ELF DWARF 调试信息解析
 * 目标: 从 .debug_info/.debug_abbrev/.debug_str 提取全局变量类型树。
 * 支持 DWARF 2/3/4(完整) 与 DWARF 5 头/常用 form(受限回退)。
 * 输出: 带成员树的全局变量表(结构体展开、位域、匿名成员合并)。
 * 位域语义: 按小端目标机换算成 bitOffset(距存储单元 LSB 的位偏移)。
 */
#include "core/variable_def.h"
#include <QByteArray>
#include <QString>
#include <QVector>

struct DwarfVar {
    QString  name;             /* 全路径: "g_motor.ctrl.kp" */
    QString  file;             /* 所属源文件(含路径), 区分跨文件同名 static */
    uint32_t addr = 0;
    uint32_t size = 0;
    VariableDef::Type type = VariableDef::Uint32;
    int      bitOffset = -1;
    int      bitWidth  = 0;
    bool     isStruct = false;
    bool     isArray  = false;   /* 数组(可展开元素) */
    QString  typeText;           /* 类型文本: 数组如 "uint8[3][4]", 空则用默认 */
    QVector<DwarfVar> members;   /* 复合类型成员(叶子时为空) */
};

class DwarfParser
{
public:
    /* elf 为整个 ELF 文件内容 */
    static bool parse(const QByteArray& elf, QVector<DwarfVar>& out,
                      QString* err = nullptr);
};

#endif /* DWARF_PARSER_H */
