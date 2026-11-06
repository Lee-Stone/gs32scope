#include "variable_writer.h"
#include "core/value_codec.h"
#include <algorithm>
#include <cmath>
#include <cstring>

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

    /* 写入后不做阻塞回读: 目标程序可能立即更新变量(计数器/节拍等),
     * 回读不一致不代表写入失败. 写事务成功即返回, 当前值由采样刷新. */
    r.readback = value;
    r.ok = true;
    return r;
}
