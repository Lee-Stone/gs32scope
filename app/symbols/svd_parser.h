#ifndef SVD_PARSER_H
#define SVD_PARSER_H
/*
 * SvdParser - CMSIS SVD(XML) 解析
 * 输出 外设→寄存器→位域 三级结构, 供符号树勾选观察。
 */
#include <QString>
#include <QVector>
#include <cstdint>

struct SvdField {
    QString name;
    int bitOffset = 0;
    int bitWidth  = 1;
};

struct SvdRegister {
    QString  name;
    uint32_t addr = 0;
    uint32_t size = 32;      /* 位 */
    QVector<SvdField> fields;
};

struct SvdPeripheral {
    QString  name;
    uint32_t baseAddr = 0;
    QVector<SvdRegister> regs;
};

class SvdParser
{
public:
    static bool parse(const QString& path, QVector<SvdPeripheral>& out,
                      QString* err = nullptr);
};

#endif /* SVD_PARSER_H */
