/*
 * 诊断工具: 对真实 ELF 运行 DwarfParser 并打印统计。
 * 用法: test_dwarf_real.exe <path.elf>
 */
#include "../app/symbols/dwarf_parser.h"
#include <QCoreApplication>
#include <QFile>
#include <cstdio>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::printf("usage: test_dwarf_real <elf>\n");
        return 1;
    }
    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("cannot open\n"); return 1; }
    const QByteArray elf = f.readAll();
    f.close();

    QVector<DwarfVar> vars;
    QString err;
    const bool ok = DwarfParser::parse(elf, vars, &err);
    std::printf("parse=%d err=[%s] vars=%d\n", int(ok), err.toUtf8().constData(),
                vars.size());

    int nStruct = 0;
    for (const auto& v : vars) {
        if (v.isStruct) ++nStruct;
        std::printf("  %s @0x%08X size=%u %s members=%d\n",
                    v.name.toUtf8().constData(), v.addr, v.size,
                    v.isStruct ? "[struct]" : "[leaf]", v.members.size());
        if (v.isStruct && !v.members.isEmpty()) {
            for (int i = 0; i < v.members.size(); ++i) {
                const auto& m = v.members[i];
                std::printf("      %s @0x%08X size=%u%s%s\n",
                            m.name.toUtf8().constData(), m.addr, m.size,
                            m.bitOffset >= 0
                                ? QString(" bit[%1:%2]")
                                      .arg(m.bitOffset + m.bitWidth - 1)
                                      .arg(m.bitOffset).toUtf8().constData()
                                : "",
                            m.isStruct ? " [struct]" : "");
            }
        }
    }
    std::printf("structs=%d\n", nStruct);
    return ok ? 0 : 1;
}
