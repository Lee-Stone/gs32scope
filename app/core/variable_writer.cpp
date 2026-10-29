#include "variable_writer.h"
#include "core/value_codec.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
QString fmt(double v, const VariableDef& d) {
    if (isFloatType(d.type) && d.bitOffset < 0)
        return QString::number(v, 'f', d.type == VariableDef::Double ? 6 : 3);
    return QString::number(static_cast<qint64>(v));
}
} // namespace

WriteResult VariableWriter::write(LinkDriver* link, const VariableDef& def, double value) {
    WriteResult r;
    if (!link || !link->isLoaded()) { r.error = QStringLiteral("驱动未加载"); return r; }
    if (def.size == 0 || def.size > 8) { r.error = QStringLiteral("变量大小非法"); return r; }
    if (!link->hasCap(GS32_CAP_WRITE)) { r.error = QStringLiteral("当前后端不支持写入"); return r; }

    uint8_t buf[8] = {};
    const uint64_t fieldMask = gs32::bitMask(def.bitWidth);
    const uint64_t wantRaw = static_cast<uint64_t>(std::llround(value));

    if (def.bitOffset >= 0) {
        /* 位域: 先读所在存储单元, 修改位段后整体写回 */
        gs32_mem_region_t rr{def.addr, def.size};
        if (!link->read(&rr, 1, buf, def.size, 0)) {
            r.error = QStringLiteral("读取原值失败: ") + link->lastErrorText();
            return r;
        }
        uint64_t raw = gs32::readRaw(def, buf);
        raw = (raw & ~(fieldMask << def.bitOffset)) |
              ((wantRaw & fieldMask) << def.bitOffset);
        std::memcpy(buf, &raw, def.size);
    } else {
        if (!gs32::encodeValue(def, value, buf)) {
            r.error = QStringLiteral("不支持的类型");
            return r;
        }
    }

    gs32_mem_region_t region{def.addr, def.size};
    if (!link->write(&region, 1, buf, def.size, 0)) {
        r.error = QStringLiteral("写入失败: ") + link->lastErrorText();
        return r;
    }

    /* 立即回读校验 */
    uint8_t rb[8] = {};
    if (!link->read(&region, 1, rb, def.size, 0)) {
        r.error = QStringLiteral("写入已发送, 但回读失败: ") + link->lastErrorText();
        return r;
    }
    if (def.bitOffset >= 0)
        r.readback = gs32::decodeBitfield(def, gs32::readRaw(def, rb));
    else
        r.readback = gs32::decodeValue(def, rb);

    bool same = false;
    if (def.bitOffset >= 0) {
        same = static_cast<uint64_t>(std::llround(r.readback)) == (wantRaw & fieldMask);
    } else if (isFloatType(def.type)) {
        const double tol = std::max(1e-6, std::fabs(value) * 1e-5);
        same = std::fabs(r.readback - value) <= tol;
    } else {
        same = static_cast<qint64>(std::llround(r.readback)) ==
               static_cast<qint64>(std::llround(value));
    }
    if (!same) {
        r.error = QStringLiteral("回读不一致(写入 %1, 回读 %2)")
                      .arg(fmt(value, def), fmt(r.readback, def));
        return r;
    }
    r.ok = true;
    return r;
}
