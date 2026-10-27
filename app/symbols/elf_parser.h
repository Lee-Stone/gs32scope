#ifndef ELF_PARSER_H
#define ELF_PARSER_H
/*
 * ElfParser - ELF 符号表(.symtab)导入
 * 支持 ELF32/ELF64、小端。DWARF 类型树解析(DwarfParser)后续叠加,
 * 届时同名符号以 DWARF 提供的类型信息为准。
 */
#include <QString>
#include <QVector>
#include <cstdint>

struct ElfSymbol {
    QString  name;
    uint32_t addr = 0;
    uint32_t size = 0;
};

class ElfParser
{
public:
    /* 解析并过滤出可观测的数据符号(STT_OBJECT, 非编译器内部) */
    static bool parse(const QString& path, QVector<ElfSymbol>& out,
                      QString* err = nullptr);
};

#endif /* ELF_PARSER_H */
