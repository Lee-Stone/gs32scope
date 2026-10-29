#ifndef VALUE_CODEC_H
#define VALUE_CODEC_H
/*
 * 变量值编解码: 采样解码与写入编码共用。
 * 目标为小端 RISC-V; 位域按 VariableDef::bitOffset(距 LSB) + bitWidth 处理。
 */
#include "core/variable_def.h"
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace gs32 {

inline uint64_t bitMask(int width) {
    if (width <= 0) return 0;
    if (width >= 64) return ~0ull;
    return (1ull << width) - 1ull;
}

inline double decodeValue(const VariableDef& v, const uint8_t* p) {
    switch (v.type) {
    case VariableDef::Double: {
        double d = 0;
        std::memcpy(&d, p, sizeof(double));
        return d;
    }
    case VariableDef::Float: {
        float f = 0;
        std::memcpy(&f, p, sizeof(float));
        return f;
    }
    case VariableDef::Int8:  return double(*reinterpret_cast<const int8_t*>(p));
    case VariableDef::Int16: { int16_t x; std::memcpy(&x, p, 2); return double(x); }
    case VariableDef::Int32: { int32_t x; std::memcpy(&x, p, 4); return double(x); }
    case VariableDef::Int64: { int64_t x; std::memcpy(&x, p, 8); return double(x); }
    case VariableDef::Uint8:  return double(*p);
    case VariableDef::Uint16: { uint16_t x; std::memcpy(&x, p, 2); return double(x); }
    case VariableDef::Uint32: { uint32_t x; std::memcpy(&x, p, 4); return double(x); }
    case VariableDef::Uint64: { uint64_t x; std::memcpy(&x, p, 8); return double(x); }
    }
    return 0;
}

/* 位域: 从已读出的存储单元原始值中取值 */
inline double decodeBitfield(const VariableDef& v, uint64_t raw) {
    if (v.bitOffset < 0 || v.bitWidth <= 0) return double(raw);
    return double((raw >> v.bitOffset) & bitMask(v.bitWidth));
}

inline uint64_t readRaw(const VariableDef& v, const uint8_t* p) {
    uint64_t raw = 0;
    const uint32_t n = std::min<uint32_t>(v.size ? v.size : 8, 8);
    std::memcpy(&raw, p, n);
    return raw;
}

inline bool encodeValue(const VariableDef& v, double value, uint8_t* p) {
    switch (v.type) {
    case VariableDef::Double: {
        const double d = value;
        std::memcpy(p, &d, sizeof(double));
        return true;
    }
    case VariableDef::Float: {
        const float f = float(value);
        std::memcpy(p, &f, sizeof(float));
        return true;
    }
    case VariableDef::Int8:  { const int8_t x = int8_t(value); std::memcpy(p, &x, 1); return true; }
    case VariableDef::Int16: { const int16_t x = int16_t(value); std::memcpy(p, &x, 2); return true; }
    case VariableDef::Int32: { const int32_t x = int32_t(value); std::memcpy(p, &x, 4); return true; }
    case VariableDef::Int64: { const int64_t x = int64_t(value); std::memcpy(p, &x, 8); return true; }
    case VariableDef::Uint8:  { const uint8_t x = uint8_t(value); std::memcpy(p, &x, 1); return true; }
    case VariableDef::Uint16: { const uint16_t x = uint16_t(value); std::memcpy(p, &x, 2); return true; }
    case VariableDef::Uint32: { const uint32_t x = uint32_t(value); std::memcpy(p, &x, 4); return true; }
    case VariableDef::Uint64: { const uint64_t x = uint64_t(value); std::memcpy(p, &x, 8); return true; }
    }
    return false;
}

/* 写入值类型: 浮点/有符号/无符号(用于输入解析) */
inline bool isSignedType(VariableDef::Type t) {
    return t == VariableDef::Int8 || t == VariableDef::Int16 ||
           t == VariableDef::Int32 || t == VariableDef::Int64;
}

} // namespace gs32

#endif /* VALUE_CODEC_H */
