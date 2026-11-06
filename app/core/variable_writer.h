#ifndef VARIABLE_WRITER_H
#define VARIABLE_WRITER_H
/*
 * VariableWriter - 单个变量的写入。
 * 位域走读-改-写; 普通类型按 VariableDef 编码。写事务成功即返回,
 * 不做阻塞回读(目标程序可能持续更新该变量)。
 */
#include "core/variable_def.h"
#include "link/link_driver.h"
#include <QString>

struct WriteResult {
    bool    ok = false;
    QString error;
    double  readback = 0.0;
};

class VariableWriter
{
public:
    static WriteResult write(LinkDriver* link, const VariableDef& def, double value);
};

#endif /* VARIABLE_WRITER_H */
