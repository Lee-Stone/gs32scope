#include "svd_parser.h"
#include <QFile>
#include <QXmlStreamReader>

bool SvdParser::parse(const QString& path, QVector<SvdPeripheral>& out,
                      QString* err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (err) *err = QStringLiteral("无法打开 SVD: %1").arg(path);
        return false;
    }

    QXmlStreamReader xs(&f);
    SvdPeripheral* curPeri = nullptr;
    SvdRegister*   curReg  = nullptr;
    /* <size>/<bitOffset>/<bitWidth> 的值先存为文本, 结束标签时落盘 */
    QString textVal;
    int pendingRole = 0;   /* 1=base 2=regOff 3=regSize 4=fldOff 5=fldWidth 6=lsb 7=msb */
    uint32_t pendU = 0;

    auto toU32 = [](const QString& s) -> uint32_t {
        QString t = s.trimmed();
        bool ok = false;
        if (t.startsWith("0x") || t.startsWith("0X"))
            return t.mid(2).toUInt(&ok, 16);
        return t.toUInt(&ok, 10);
    };

    while (!xs.atEnd()) {
        const auto type = xs.readNext();
        if (type == QXmlStreamReader::StartElement) {
            const QStringView n = xs.name();
            if (n == QStringLiteral("peripheral")) {
                out.append(SvdPeripheral{});
                curPeri = &out.last();
            } else if (curPeri && n == QStringLiteral("register")) {
                curPeri->regs.append(SvdRegister{});
                curReg = &curPeri->regs.last();
            } else if (curReg && n == QStringLiteral("field")) {
                curReg->fields.append(SvdField{});
            } else if (n == QStringLiteral("name")) {
                textVal = xs.readElementText();
                if (curReg && !curReg->fields.isEmpty() &&
                    curReg->fields.last().name.isEmpty())
                    curReg->fields.last().name = textVal.trimmed();
                else if (curReg && curReg->name.isEmpty())
                    curReg->name = textVal.trimmed();
                else if (curPeri && curPeri->name.isEmpty())
                    curPeri->name = textVal.trimmed();
            } else if (n == QStringLiteral("baseAddress") && curPeri) {
                curPeri->baseAddr = toU32(xs.readElementText());
            } else if (n == QStringLiteral("addressOffset") && curReg) {
                pendU = toU32(xs.readElementText());
                curReg->addr = curPeri->baseAddr + pendU;
            } else if (n == QStringLiteral("size") && curReg &&
                       curReg->fields.isEmpty()) {
                curReg->size = toU32(xs.readElementText());
            } else if (n == QStringLiteral("bitOffset") && curReg &&
                       !curReg->fields.isEmpty()) {
                curReg->fields.last().bitOffset = (int)toU32(xs.readElementText());
            } else if (n == QStringLiteral("bitWidth") && curReg &&
                       !curReg->fields.isEmpty()) {
                curReg->fields.last().bitWidth = (int)toU32(xs.readElementText());
            } else if (n == QStringLiteral("lsb") && curReg &&
                       !curReg->fields.isEmpty()) {
                curReg->fields.last().bitOffset = (int)toU32(xs.readElementText());
            } else if (n == QStringLiteral("msb") && curReg &&
                       !curReg->fields.isEmpty()) {
                /* width = msb - lsb + 1, lsb 已在 bitOffset */
                curReg->fields.last().bitWidth =
                    (int)toU32(xs.readElementText()) -
                    curReg->fields.last().bitOffset + 1;
            }
        }
    }
    f.close();

    if (xs.hasError()) {
        if (err) *err = QStringLiteral("SVD 解析错误: %1").arg(xs.errorString());
        return false;
    }
    if (err) *err = QString();
    return true;
}
