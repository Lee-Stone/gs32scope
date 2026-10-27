#include "dwarf_parser.h"
#include <QMap>
#include <QSet>
#include <QStack>
#include <cstring>
#include <functional>

/* ================= DWARF 常量 ================= */
namespace dw {

enum : uint16_t {
    TAG_array_type = 0x01,  TAG_enumeration_type = 0x04, TAG_member = 0x0d,
    TAG_pointer_type = 0x0f, TAG_compile_unit = 0x11,
    TAG_structure_type = 0x13, TAG_subroutine_type = 0x15, TAG_typedef = 0x16,
    TAG_union_type = 0x17, TAG_unspecified_parameters = 0x18,
    TAG_variant = 0x19, TAG_inheritance = 0x1c,
    TAG_subrange_type = 0x21, TAG_base_type = 0x24,
    TAG_const_type = 0x26, TAG_enumerator = 0x28,
    TAG_subprogram = 0x2e, TAG_variable = 0x34, TAG_volatile_type = 0x35,
    TAG_restrict_type = 0x37, TAG_namespace = 0x39,
    TAG_rvalue_reference_type = 0x42, TAG_atomic_type = 0x47,
};

enum : uint16_t {
    AT_location = 0x02, AT_name = 0x03, AT_byte_size = 0x0b,
    AT_bit_offset = 0x0c, AT_bit_size = 0x0d, AT_stmt_list = 0x10,
    AT_language = 0x13, AT_upper_bound = 0x2f, AT_abstract_origin = 0x31,
    AT_count = 0x37, AT_data_member_location = 0x38,
    AT_declaration = 0x3c, AT_encoding = 0x3e, AT_external = 0x3f,
    AT_specification = 0x47, AT_type = 0x49, AT_ranges = 0x55,
    AT_comp_dir = 0x1b,
    AT_data_bit_offset = 0x6b, AT_str_offsets_base = 0x72, AT_addr_base = 0x73,
};

enum : uint64_t {
    OP_addr = 0x03, OP_plus_uconst = 0x23,
    OP_addrx = 0x94, OP_GNU_addr_index = 0xfb,
};

enum : uint8_t {
    FORM_addr = 0x01, FORM_block2 = 0x03, FORM_block4 = 0x04,
    FORM_data2 = 0x05, FORM_data4 = 0x06, FORM_data8 = 0x07,
    FORM_string = 0x08, FORM_block = 0x09, FORM_block1 = 0x0a,
    FORM_data1 = 0x0b, FORM_flag = 0x0c, FORM_sdata = 0x0d,
    FORM_strp = 0x0e, FORM_udata = 0x0f, FORM_ref_addr = 0x10,
    FORM_ref1 = 0x11, FORM_ref2 = 0x12, FORM_ref4 = 0x13, FORM_ref8 = 0x14,
    FORM_ref_udata = 0x15, FORM_indirect = 0x16, FORM_sec_offset = 0x17,
    FORM_exprloc = 0x18, FORM_flag_present = 0x19, FORM_strx = 0x1a,
    FORM_addrx = 0x1b, FORM_data16 = 0x1e, FORM_line_strp = 0x1f,
    FORM_implicit_const = 0x21, FORM_loclistx = 0x22, FORM_rnglistx = 0x23,
    FORM_strx1 = 0x25, FORM_strx2 = 0x26, FORM_strx3 = 0x27, FORM_strx4 = 0x28,
    FORM_addrx1 = 0x29, FORM_addrx2 = 0x2a, FORM_addrx3 = 0x2b, FORM_addrx4 = 0x2c,
};

enum : uint8_t { ATE_boolean = 2, ATE_float = 4, ATE_signed = 5,
                 ATE_signed_char = 6, ATE_unsigned = 7, ATE_unsigned_char = 8 };

/* ================= 字节读取 ================= */
class Rd {
public:
    Rd(const uint8_t* p, int n) : p_(p), n_(n) {}
    bool eof() const { return pos_ >= n_; }
    int  pos() const { return pos_; }
    int  len() const { return n_; }
    void seek(int pos) { pos_ = pos; }
    uint8_t  u8()  { return p_[pos_++]; }
    uint16_t u16() { uint16_t v; memcpy(&v, p_ + pos_, 2); pos_ += 2; return v; }
    uint32_t u32() { uint32_t v; memcpy(&v, p_ + pos_, 4); pos_ += 4; return v; }
    uint64_t u64() { uint64_t v; memcpy(&v, p_ + pos_, 8); pos_ += 8; return v; }
    uint64_t uleb() {
        uint64_t r = 0; int sh = 0; uint8_t b;
        do { b = u8(); r |= uint64_t(b & 0x7f) << sh; sh += 7; } while (b & 0x80);
        return r;
    }
    int64_t sleb() {
        int64_t r = 0; int sh = 0; uint8_t b;
        do { b = u8(); r |= int64_t(b & 0x7f) << sh; sh += 7; } while (b & 0x80);
        if (sh < 64 && (b & 0x40)) r |= -(int64_t(1) << sh);
        return r;
    }
    QByteArray bytes(int n) { QByteArray b(reinterpret_cast<const char*>(p_ + pos_), n); pos_ += n; return b; }
    QString cstr() {
        const char* s = reinterpret_cast<const char*>(p_ + pos_);
        const int len = int(strnlen(s, n_ - pos_));
        pos_ += len + 1;
        return QString::fromUtf8(s, len);
    }
    bool skip(int n) { if (pos_ + n > n_) return false; pos_ += n; return true; }
private:
    const uint8_t* p_; int n_; int pos_ = 0;
};

/* ================= 属性值 ================= */
struct Val {
    enum Kind { Uint, Str, Bytes, Ref } kind = Uint;
    uint64_t u = 0;
    QString  s;
    QByteArray b;
};

/* ================= DIE ================= */
struct Die {
    uint32_t offset = 0;
    uint16_t tag = 0;
    bool hasChildren = false;
    QMap<uint16_t, Val> attrs;
    QVector<Die*> children;
    Die* parent = nullptr;
};

struct Abbrev {
    uint16_t tag = 0;
    bool hasChildren = false;
    QVector<QPair<uint16_t, uint64_t>> specs;   /* attr, form(或 implicit_const 值) */
};

/* ================= ELF section 提取 ================= */
bool getSections(const QByteArray& elf, bool is64,
                 QMap<QString, QByteArray>& out) {
    auto rdAt = [&](qint64 off, int n) {
        return QByteArray::fromRawData(elf.constData() + off, n);
    };
    uint64_t shoff, shentsize; uint16_t shnum, shstrndx;
    if (is64) {
        if (elf.size() < 64) return false;
        const auto* eh = reinterpret_cast<const uint8_t*>(elf.constData());
        Rd r(eh, 64);
        r.skip(0x28);
        shoff = r.u64(); /* e_shoff */
        r.u32();               /* e_flags */
        r.skip(2 + 2);         /* e_ehsize, e_phentsize */
        r.u16();               /* e_phnum */
        shentsize = r.u16(); shnum = r.u16(); shstrndx = r.u16();
        if (shentsize != 64) return false;
    } else {
        if (elf.size() < 52) return false;
        const auto* eh = reinterpret_cast<const uint8_t*>(elf.constData());
        Rd r(eh, 52);
        r.skip(0x20);
        shoff = r.u32();
        r.u32();               /* e_flags */
        r.skip(2 + 2);         /* e_ehsize, e_phentsize */
        r.u16();               /* e_phnum */
        shentsize = r.u16(); shnum = r.u16(); shstrndx = r.u16();
        if (shentsize != 40) return false;
    }
    if (shoff == 0 || shnum == 0 || shoff + uint64_t(shnum) * shentsize > uint64_t(elf.size()))
        return false;

    auto readSh = [&](int i, uint32_t& type, uint32_t& link,
                      uint64_t& off, uint64_t& size) {
        const qint64 o = qint64(shoff) + qint64(i) * shentsize;
        const QByteArray b = rdAt(o, int(shentsize));
        Rd r(reinterpret_cast<const uint8_t*>(b.constData()), b.size());
        uint32_t name = r.u32(); (void)name;
        type = r.u32();
        if (is64) { r.u64(); off = r.u64(); size = r.u64(); link = r.u32(); r.skip(20); }
        else { r.u32(); r.u32(); off = r.u32(); size = r.u32(); link = r.u32(); r.skip(12); }
    };

    uint32_t t, l; uint64_t o, s;
    readSh(shstrndx, t, l, o, s);
    const QByteArray shstr = rdAt(qint64(o), qint64(s));

    auto cstrAt = [](const QByteArray& d, uint32_t off) {
        if (int(off) >= d.size()) return QString();
        return QString::fromUtf8(d.constData() + off);
    };
    for (int i = 0; i < shnum; ++i) {
        readSh(i, t, l, o, s);
        if (t == 8 /*NOBITS*/ || s == 0) continue;
        const QString name = cstrAt(shstr,
            [&]{ Rd rr(reinterpret_cast<const uint8_t*>(rdAt(qint64(shoff) + qint64(i) * shentsize, int(shentsize)).constData()), int(shentsize)); return rr.u32(); }());
        out.insert(name, rdAt(qint64(o), qint64(s)));
    }
    return true;
}

} // namespace dw

/* ================= 主解析器 ================= */
using namespace dw;

namespace {

struct CType {
    enum Kind { Unknown, Base, Struct, Union, Enum, Ptr, Array, Typedef } kind = Unknown;
    QString name;
    uint64_t size = 0;
    bool isSigned = false;
    CType* elem = nullptr;             /* array/ptr 元素类型 */
    QVector<uint64_t> dims;            /* 数组各维大小(外层->内层) */
    struct Member {
        QString name;
        CType*  type = nullptr;
        uint64_t byteOff = 0;
        bool hasBits = false;
        uint32_t bitOff = 0;           /* 距存储单元 LSB 的位偏移(小端语义) */
        uint32_t bitSize = 0;
    };
    QVector<Member> members;
};

struct Ctx {
    const QMap<QString, QByteArray>* secs = nullptr;
    QMap<uint64_t, Abbrev> abbrevs;
    QVector<Die*> allDies;             /* 拥有所有权 */
    QMap<uint32_t, Die*> byOffset;     /* CU 内相对偏移 -> DIE */
    QMap<Die*, CType*> typeCache;      /* 类型解析缓存(与 allDies 同生命周期) */
    QSet<CType*> ownedTypes;           /* 真正拥有的类型(共享指针只记一次) */
    uint16_t version = 4;
    uint8_t  addrSize = 4;
    uint64_t strOffsetsBase = 0;
    uint64_t addrBase = 0;
    bool     hasStrOffsetsBase = false;
    const QByteArray* debugAddr = nullptr;
    ~Ctx() { qDeleteAll(allDies); for (CType* t : ownedTypes) delete t; }
};

const QByteArray* sec(const Ctx& c, const char* n) {
    auto it = c.secs->constFind(QString::fromLatin1(n));
    return it == c.secs->constEnd() ? nullptr : &it.value();
}

/* ---- abbrev ---- */
bool parseAbbrev(Ctx& c, QString* err) {
    const QByteArray* d = sec(c, ".debug_abbrev");
    if (!d || d->isEmpty()) { if (err) *err = "缺少 .debug_abbrev"; return false; }
    Rd r(reinterpret_cast<const uint8_t*>(d->constData()), d->size());
    while (!r.eof()) {
        const uint64_t code = r.uleb();
        if (code == 0) break;                        /* 表终止符(段内含多张表) */
        Abbrev a;
        a.tag = uint16_t(r.uleb());
        a.hasChildren = r.u8() != 0;
        for (;;) {
            const uint64_t at = r.uleb();
            const uint64_t form = r.uleb();
            if (at == 0 && form == 0) break;
            uint64_t ic = 0;
            if (form == FORM_implicit_const) ic = r.sleb();
            a.specs.append({uint16_t(at), form});
            if (form == FORM_implicit_const) a.specs.last().second |= uint64_t(ic) << 32;
        }
        c.abbrevs.insert(code, a);
    }
    return true;
}

/* ---- 表单读取 ---- */
Val readForm(Rd& r, uint64_t form, uint64_t implicit, Ctx& c, uint32_t cuBase) {
    Val v;
    switch (form) {
    case FORM_addr:          v.kind = Val::Uint; v.u = (c.addrSize == 8) ? r.u64() : r.u32(); break;
    case FORM_data1: case FORM_flag: v.kind = Val::Uint; v.u = r.u8(); break;
    case FORM_data2:         v.kind = Val::Uint; v.u = r.u16(); break;
    case FORM_data4:         v.kind = Val::Uint; v.u = r.u32(); break;
    case FORM_data8:         v.kind = Val::Uint; v.u = r.u64(); break;
    case FORM_sdata:         v.kind = Val::Uint; v.u = uint64_t(r.sleb()); break;
    case FORM_udata:         v.kind = Val::Uint; v.u = r.uleb(); break;
    case FORM_string:        v.kind = Val::Str;  v.s = r.cstr(); break;
    case FORM_strp: case FORM_line_strp: {
        const uint64_t off = (c.version >= 5 || true) ? r.u32() : 0;
        const QByteArray* s = sec(c, form == FORM_line_strp ? ".debug_line_str" : ".debug_str");
        if (s && int(off) < s->size()) {
            const char* p = s->constData() + off;
            v.kind = Val::Str;
            v.s = QString::fromUtf8(p, strnlen(p, s->size() - int(off)));
        }
        break;
    }
    case FORM_block1: v.kind = Val::Bytes; v.b = r.bytes(int(r.u8())); break;
    case FORM_block2: v.kind = Val::Bytes; v.b = r.bytes(int(r.u16())); break;
    case FORM_block4: v.kind = Val::Bytes; v.b = r.bytes(int(r.u32())); break;
    case FORM_block: case FORM_exprloc: {
        const int n = int(r.uleb());
        v.kind = Val::Bytes; v.b = r.bytes(n); break;
    }
    case FORM_data16:   v.kind = Val::Bytes; v.b = r.bytes(16); break;
    case FORM_ref1: v.kind = Val::Ref; v.u = cuBase + r.u8(); break;
    case FORM_ref2: v.kind = Val::Ref; v.u = cuBase + r.u16(); break;
    case FORM_ref4: v.kind = Val::Ref; v.u = cuBase + r.u32(); break;
    case FORM_ref8: v.kind = Val::Ref; v.u = cuBase + r.u64(); break;
    case FORM_ref_udata: v.kind = Val::Ref; v.u = cuBase + r.uleb(); break;
    case FORM_ref_addr:  v.kind = Val::Ref; v.u = r.u32(); break;   /* 简化: 32 位 DWARF */
    case FORM_sec_offset: v.kind = Val::Uint; v.u = r.u32(); break;
    case FORM_flag_present: v.kind = Val::Uint; v.u = 1; break;
    case FORM_strx: case FORM_addrx: case FORM_loclistx: case FORM_rnglistx:
        v.kind = Val::Uint; v.u = r.uleb(); break;   /* 索引延迟解析 */
    case FORM_strx1: case FORM_addrx1: v.kind = Val::Uint; v.u = r.u8(); break;
    case FORM_strx2: case FORM_addrx2: v.kind = Val::Uint; v.u = r.u16(); break;
    case FORM_strx3: case FORM_addrx3: v.kind = Val::Uint; { uint32_t x = 0;
        memcpy(&x, (r.bytes(3)).constData(), 3); v.u = x; } break;
    case FORM_strx4: case FORM_addrx4: v.kind = Val::Uint; v.u = r.u32(); break;
    case FORM_implicit_const: v.kind = Val::Uint; v.u = uint32_t(implicit); break;
    case FORM_indirect: {
        const uint64_t f2 = r.uleb();
        return readForm(r, f2, implicit, c, cuBase);
    }
    default: v.kind = Val::Uint; v.u = 0; break;   /* 未知 form: 占位 */
    }
    return v;
}

/* ---- CU DIE 解析 ---- */
bool parseInfo(Ctx& c, QString* err) {
    const QByteArray* info = sec(c, ".debug_info");
    if (!info || info->isEmpty()) { if (err) *err = "缺少 .debug_info"; return false; }
    if (!parseAbbrev(c, err)) return false;
    const QByteArray* strSec = sec(c, ".debug_str");

    Rd r(reinterpret_cast<const uint8_t*>(info->constData()), info->size());
    while (!r.eof() && r.pos() + 4 <= info->size()) {
        const uint32_t cuStart = uint32_t(r.pos());
        const uint32_t cuLen = r.u32();
        if (cuLen == 0xFFFFFFFF) { if (err) *err = "暂不支持 DWARF64"; return false; }
        const uint32_t cuEnd = uint32_t(r.pos()) + cuLen;
        c.version = r.u16();
        if (c.version < 2 || c.version > 5) { if (err) *err = "不支持的 DWARF 版本"; return false; }
        uint64_t abbrevOff = 0;
        uint8_t unitType = 0;
        if (c.version >= 5) {
            unitType = r.u8();
            c.addrSize = r.u8();
            abbrevOff = r.u32();
            if (unitType != 1 /*compile*/) { r.seek(cuEnd); continue; }
        } else {
            abbrevOff = r.u32();
            c.addrSize = r.u8();
        }
        c.abbrevs.clear();
        /* abbrevOff 指向 .debug_abbrev; 重新解析该偏移处的表 */
        {
            const QByteArray* ad = sec(c, ".debug_abbrev");
            Rd ar(reinterpret_cast<const uint8_t*>(ad->constData()) + abbrevOff,
                  ad->size() - int(abbrevOff));
            int dbgN = 0;
            while (!ar.eof()) {
                const uint64_t code = ar.uleb();
                if (code == 0) break;   /* 表终止符(段内可能有多张表, 只取本表) */
                Abbrev a;
                a.tag = uint16_t(ar.uleb());
                a.hasChildren = ar.u8() != 0;
                for (;;) {
                    const uint64_t at = ar.uleb();
                    const uint64_t form = ar.uleb();
                    if (at == 0 && form == 0) break;
                    uint64_t ic = 0;
                    if (form == FORM_implicit_const) ic = uint64_t(ar.sleb());
                    a.specs.append({uint16_t(at), (ic << 32) | form});
                }
                c.abbrevs.insert(code, a);
            }
        }

        const int cuDieStart = c.allDies.size();
        c.hasStrOffsetsBase = false;
        QStack<Die*> stack;
        while (r.pos() < int(cuEnd)) {
            const uint32_t dieOff = uint32_t(r.pos());   /* 绝对偏移(跨 CU 引用统一) */
            const uint64_t code = r.uleb();
            if (code == 0) {                      /* 子列表结束 */
                if (!stack.isEmpty()) stack.pop();
                continue;
            }
            const Abbrev ab = c.abbrevs.value(code);
            if (ab.tag == 0) {
                char b[16]; snprintf(b, sizeof(b), "0x%llX", (unsigned long long)code);
                *err = QStringLiteral("未知 abbrev code %1 @dieOff=%2 abbrevs=%3")
                           .arg(b).arg(dieOff).arg(c.abbrevs.size());
                std::printf("[dbg] %s\n", err->toUtf8().constData());
                return false;
            }
            auto* die = new Die();
            die->offset = dieOff;
            die->tag = ab.tag;
            die->hasChildren = ab.hasChildren;
            for (const auto& sp : ab.specs) {
                const uint64_t form = sp.second & 0xFFFFFFFFull;
                const uint64_t ic  = sp.second >> 32;
                die->attrs.insert(sp.first, readForm(r, form, ic, c, cuStart));
            }
            c.allDies.append(die);
            c.byOffset.insert(dieOff, die);
            if (!stack.isEmpty()) { die->parent = stack.top(); stack.top()->children.append(die); }
            if (die->hasChildren) stack.push(die);
            /* DWARF5: str_offsets_base / addr_base 在 CU 根 DIE 上 */
            if (c.version >= 5 && die->tag == TAG_compile_unit) {
                if (die->attrs.contains(AT_str_offsets_base)) {
                    c.strOffsetsBase = die->attrs[AT_str_offsets_base].u;
                    c.hasStrOffsetsBase = true;
                }
                if (die->attrs.contains(AT_addr_base))
                    c.addrBase = die->attrs[AT_addr_base].u;
            }
        }
        /* 回填 strx 名称(仅本 CU, 有 str_offsets_base 时) */
        if (c.hasStrOffsetsBase && strSec) {
            for (int i = cuDieStart; i < c.allDies.size(); ++i) {
                Die* d = c.allDies[i];
                auto it = d->attrs.find(AT_name);
                if (it == d->attrs.end() || it->kind != Val::Uint) continue;
                const uint64_t idx = it->u;
                const uint64_t off = c.strOffsetsBase + idx * 4;
                if (int(off) + 4 <= strSec->size()) {
                    uint32_t so; memcpy(&so, strSec->constData() + off, 4);
                    if (int(so) < strSec->size()) {
                        const char* p = strSec->constData() + so;
                        it->kind = Val::Str;
                        it->s = QString::fromUtf8(p, strnlen(p, strSec->size() - int(so)));
                    }
                }
            }
        }
        r.seek(cuEnd);   /* 全程单 Rd, pos 恒为绝对偏移 */
    }
    return true;
}

/* ---- 类型解析 ---- */
CType* resolveType(Die* die, Ctx& c, int depth = 0);

CType* makeType(Die* die, Ctx& c, int depth) {
    auto* t = new CType();
    const auto at = [&](uint16_t a) -> Val* {
        auto it = die->attrs.find(a);
        return it == die->attrs.end() ? nullptr : &it.value();
    };
    if (auto* n = at(AT_name)) t->name = n->s;
    if (auto* s = at(AT_byte_size)) t->size = s->u;

    switch (die->tag) {
    case TAG_base_type: {
        t->kind = CType::Base;
        uint8_t enc = 0;
        if (auto* e = at(AT_encoding)) enc = uint8_t(e->u);
        t->isSigned = (enc == ATE_signed || enc == ATE_signed_char);
        if (enc == ATE_float) t->name = t->name.isEmpty() ? "float" : t->name;
        break;
    }
    case TAG_typedef: case TAG_volatile_type: case TAG_const_type:
    case TAG_restrict_type: case TAG_atomic_type: {
        t->kind = CType::Typedef;
        if (auto* ty = at(AT_type)) {
            if (ty->kind == Val::Ref) {
                if (Die* target = c.byOffset.value(uint32_t(ty->u)))
                    return resolveType(target, c, depth + 1);
            }
        }
        t->kind = CType::Unknown;
        break;
    }
    case TAG_pointer_type: {
        t->kind = CType::Ptr;
        t->size = c.addrSize;
        if (auto* ty = at(AT_type))
            if (ty->kind == Val::Ref)
                if (Die* target = c.byOffset.value(uint32_t(ty->u)))
                    t->elem = resolveType(target, c, depth + 1);
        break;
    }
    case TAG_structure_type: case TAG_union_type: case TAG_enumeration_type: {
        t->kind = die->tag == TAG_enumeration_type ? CType::Enum
                : die->tag == TAG_union_type ? CType::Union : CType::Struct;
        if (die->attrs.contains(AT_declaration)) return t;
        for (Die* ch : die->children) {
            if (ch->tag != TAG_member) continue;
            auto nm = ch->attrs.find(AT_name);
            CType::Member m;
            m.name = nm == ch->attrs.end() ? QString() : nm->s;
            if (auto ty = ch->attrs.find(AT_type); ty != ch->attrs.end() && ty->kind == Val::Ref)
                if (Die* td = c.byOffset.value(uint32_t(ty->u)))
                    m.type = resolveType(td, c, depth + 1);
            /* data_member_location: 常量或 exprloc(DW_OP_plus_uconst) */
            if (auto loc = ch->attrs.find(AT_data_member_location);
                loc != ch->attrs.end()) {
                if (loc->kind == Val::Uint) m.byteOff = loc->u;
                else if (loc->kind == Val::Bytes && !loc->b.isEmpty()) {
                    Rd e(reinterpret_cast<const uint8_t*>(loc->b.constData()), loc->b.size());
                    uint64_t off = 0;
                    while (!e.eof()) {
                        const uint8_t op = e.u8();
                        if (op == OP_plus_uconst) { off += e.uleb(); break; }
                        if (op == OP_addr) { e.skip(int(c.addrSize)); off = 0; }
                    }
                    m.byteOff = off;
                }
            }
            if (auto bs = ch->attrs.find(AT_bit_size); bs != ch->attrs.end()) {
                m.hasBits = true;
                m.bitSize = uint32_t(bs->u);
                if (auto dbo = ch->attrs.find(AT_data_bit_offset);
                    dbo != ch->attrs.end()) {
                    /* DWARF5: 距结构体起始的位偏移(小端语义) */
                    const uint32_t unitBits = m.type ? uint32_t(m.type->size * 8) : 32;
                    const uint64_t abs = dbo->u;
                    const uint32_t unitBase = uint32_t(abs / unitBits) * unitBits;
                    m.byteOff = unitBase / 8;
                    m.bitOff = uint32_t(abs - unitBase);
                } else if (auto bo = ch->attrs.find(AT_bit_offset);
                           bo != ch->attrs.end()) {
                    /* DWARF<=4: bit_offset 从存储单元 MSB 侧计(小端) */
                    const uint32_t unitBits = m.type ? uint32_t(m.type->size * 8) : 32;
                    m.bitOff = uint32_t(unitBits - bo->u - m.bitSize);
                }
            }
            /* 匿名成员(无名字的嵌套 struct/union): 展开合并进父级 */
            if (m.name.isEmpty() && m.type &&
                (m.type->kind == CType::Struct || m.type->kind == CType::Union)) {
                for (const auto& sub : m.type->members) {
                    CType::Member s2 = sub;
                    s2.byteOff += m.byteOff;
                    /* 位域的 byteOff 已含存储单元基址(见 buildNode), 此处仅并集偏移 */
                    if (s2.hasBits) s2.byteOff = sub.byteOff + m.byteOff;
                    t->members.append(s2);
                }
            } else {
                t->members.append(m);
            }
        }
        break;
    }
    case TAG_array_type: {
        t->kind = CType::Array;
        if (auto* ty = at(AT_type))
            if (ty->kind == Val::Ref)
                if (Die* target = c.byOffset.value(uint32_t(ty->u)))
                    t->elem = resolveType(target, c, depth + 1);
        /* 收集各维大小(支持多维) */
        for (Die* ch : die->children) {
            if (ch->tag != TAG_subrange_type) continue;
            uint64_t count = 0;
            if (auto cnt = ch->attrs.find(AT_count); cnt != ch->attrs.end())
                count = cnt->u;
            else if (auto ub = ch->attrs.find(AT_upper_bound); ub != ch->attrs.end())
                count = ub->u + 1;
            if (count) t->dims.append(count);
        }
        const uint64_t es = t->elem ? t->elem->size : 0;
        uint64_t total = 1;
        for (auto d : t->dims) total *= d;
        if (es) t->size = es * (total ? total : 1);
        break;
    }
    default: t->kind = CType::Unknown; break;
    }
    return t;
}

CType* resolveType(Die* die, Ctx& c, int depth) {
    if (!die || depth > 32) return nullptr;
    if (c.typeCache.contains(die)) return c.typeCache[die];
    /* 防环: 先占位 */
    auto* placeholder = new CType();
    placeholder->kind = CType::Unknown;
    c.typeCache.insert(die, placeholder);
    CType* t = makeType(die, c, depth);
    if (t != placeholder) {
        c.typeCache[die] = t;
        if (t && !c.ownedTypes.contains(t)) c.ownedTypes.insert(t);
        delete placeholder;
    }
    return t;
}

/* ---- 类型映射到 VariableDef::Type ---- */
VariableDef::Type mapType(const CType* t) {
    if (!t) return VariableDef::Uint32;
    if (t->kind == CType::Base) {
        const QString& n = t->name;
        if (n == "float")  return VariableDef::Float;
        if (n == "double") return VariableDef::Float;
        if (n == "bool" || n == "_Bool") return VariableDef::Uint8;
        if (n.contains("unsigned char")) return VariableDef::Uint8;
        if (n.contains("signed char") || n == "char") return VariableDef::Int8;
        if (n == "short" || n == "short int" || n == "signed short") return VariableDef::Int16;
        if (n.contains("unsigned short")) return VariableDef::Uint16;
        if (n == "int" || n == "signed" || n == "long" || n.contains("long int"))
            return VariableDef::Int32;
        if (n.contains("unsigned")) return VariableDef::Uint32;
        /* 按 encoding */
        if (t->isSigned) return t->size == 4 ? VariableDef::Int32 : VariableDef::Uint32;
        return VariableDef::Uint32;
    }
    if (t->kind == CType::Enum) return VariableDef::Uint32;
    if (t->kind == CType::Ptr) return VariableDef::Uint32;
    switch (t->size) {
    case 1: return VariableDef::Uint8;
    case 2: return VariableDef::Uint16;
    default: return VariableDef::Uint32;
    }
}

/* ---- 数组元素展开(行优先, 支持多维/嵌套/结构体元素; 超过 100 元素自动分页) ---- */
void buildNode(const QString& name, uint32_t addr, uint64_t byteOff,
               const CType* t, DwarfVar& out, int depth);   /* 前向声明 */
void buildArrayEls(const QString& baseName, uint32_t addr, const CType* t,
                   QVector<DwarfVar>& out, int depth);       /* 前向声明 */

static uint64_t arrayStride(const CType* t, int depth) {
    const uint64_t es = t->elem && t->elem->size ? t->elem->size : 1;
    uint64_t tail = 1;
    for (int k = depth + 1; k < int(t->dims.size()); ++k) tail *= t->dims[k];
    return tail * es;   /* 当前维一个元素占用的字节(行优先) */
}

/* 元素类型显示名: 枚举用枚举类型名, 结构体/联合体/指针用类别, 基础类型用映射名 */
static QString elemDisplayName(const CType* el) {
    if (!el) return "?";
    switch (el->kind) {
    case CType::Enum:   return el->name.isEmpty() ? QStringLiteral("enum") : el->name;
    case CType::Struct: return QStringLiteral("struct");
    case CType::Union:  return QStringLiteral("union");
    case CType::Ptr:    return QStringLiteral("ptr");
    case CType::Array:  return elemDisplayName(el->elem);
    default:            return QString::fromLatin1(typeName(mapType(el)));
    }
}

static QString arrayTypeText(const CType* t, int depth) {
    QString tt = elemDisplayName(t->elem);
    for (int k = depth + 1; k < int(t->dims.size()); ++k)
        tt += QStringLiteral("[%1]").arg(t->dims[k]);
    return tt;
}

/* 生成 [lo..hi] 下标的元素 */
static void fillArrayRange(const QString& baseName, uint32_t baseAddr,
                           const CType* t, int depth,
                           uint64_t lo, uint64_t hi, QVector<DwarfVar>& out) {
    const CType* el = t->elem;
    const uint64_t es = el && el->size ? el->size : 1;
    const uint64_t stride = arrayStride(t, depth);
    for (uint64_t i = lo; i <= hi; ++i) {
        DwarfVar c;
        c.name = QStringLiteral("%1[%2]").arg(baseName).arg(i);
        c.addr = baseAddr + uint32_t((i - lo) * stride);
        if (el && el->kind == CType::Array) {
            /* 嵌套数组类型(int a[3][4] 由内层 array_type 表示) */
            c.isArray = true; c.isStruct = true;
            const CType* inn = el->elem;
            c.type = inn ? mapType(inn) : VariableDef::Uint32;
            c.size = uint32_t(el->size);
            QString tt = elemDisplayName(inn);
            for (auto d : el->dims) tt += QStringLiteral("[%1]").arg(d);
            c.typeText = tt;
            buildArrayEls(c.name, c.addr, el, c.members, 0);
        } else if (depth + 1 < int(t->dims.size())) {
            /* 非嵌套多维: 同一类型剩余维 */
            c.isArray = true; c.isStruct = true;
            c.type = el ? mapType(el) : VariableDef::Uint32;
            c.size = uint32_t(stride);
            c.typeText = arrayTypeText(t, depth);
            buildArrayEls(c.name, c.addr, t, c.members, depth + 1);
        } else if (el && (el->kind == CType::Struct || el->kind == CType::Union)) {
            /* 结构体元素: 展开其成员 */
            buildNode(c.name, c.addr, 0, el, c, depth + 1);
        } else {
            /* 基础类型元素(叶子) */
            c.size = uint32_t(es);
            c.type = el ? mapType(el) : VariableDef::Uint32;
            if (el && el->kind == CType::Enum) c.typeText = el->name;   /* 枚举元素显示类型名 */
        }
        out.append(c);
    }
}

void buildArrayEls(const QString& baseName, uint32_t addr, const CType* t,
                   QVector<DwarfVar>& out, int depth) {
    if (!t || t->kind != CType::Array) return;
    if (depth < 0 || depth >= int(t->dims.size()) || depth > 6) return;
    const uint64_t dim = t->dims[depth];
    if (!dim) return;

    constexpr uint64_t kPage = 100;   /* 超过 100 元素按页分组, 避免一次展开过多节点 */
    if (dim > kPage) {
        const uint64_t stride = arrayStride(t, depth);
        for (uint64_t s = 0; s < dim; s += kPage) {
            const uint64_t e = qMin(s + kPage, dim) - 1;
            DwarfVar grp;
            grp.name = QStringLiteral("%1[%2..%3]").arg(baseName).arg(e).arg(s);
            grp.isArray = true; grp.isStruct = true;
            grp.type = t->elem ? mapType(t->elem) : VariableDef::Uint32;
            grp.size = uint32_t((e - s + 1) * stride);
            grp.typeText = arrayTypeText(t, depth);
            fillArrayRange(baseName, addr + uint32_t(s * stride),
                           t, depth, s, e, grp.members);
            out.append(grp);
        }
        return;
    }
    fillArrayRange(baseName, addr, t, depth, 0, dim - 1, out);
}

/* ---- 变量树构建 ---- */
void buildNode(const QString& name, uint32_t addr, uint64_t byteOff,
               const CType* t, DwarfVar& out, int depth) {
    if (!t || depth > 16) return;
    out.name = name;
    out.addr = addr + uint32_t(byteOff);
    out.bitOffset = -1;
    switch (t->kind) {
    case CType::Enum:
        out.type = mapType(t);
        out.size = uint32_t(t->size ? t->size : 4);
        out.isStruct = false;
        out.typeText = t->name;   /* 枚举类型名(如 STATE), 供 UI 类型列显示 */
        break;
    case CType::Base: case CType::Ptr:
        out.type = mapType(t);
        out.size = uint32_t(t->size ? t->size : 4);
        out.isStruct = false;
        break;
    case CType::Struct: case CType::Union: {
        out.isStruct = true;
        out.size = uint32_t(t->size);
        out.type = VariableDef::Uint32;
        for (const auto& m : t->members) {
            DwarfVar child;
            if (m.hasBits) {
                /* 存储单元 = m.type 的大小; addr 指向单元基址(含嵌套累计偏移) */
                const uint32_t unitBytes = m.type ? uint32_t(m.type->size) : 4;
                child.name = name + "." + m.name;
                child.addr = addr + uint32_t(byteOff + m.byteOff);
                child.size = unitBytes;
                child.type = unitBytes == 2 ? VariableDef::Uint16 : VariableDef::Uint32;
                child.bitOffset = m.bitOff;
                child.bitWidth  = m.bitSize;
                child.isStruct = false;
                out.members.append(child);
            } else if (m.name.isEmpty()) {
                /* 已在类型层合并, 理论不会再出现 */
            } else {
                buildNode(name + "." + m.name, addr, byteOff + m.byteOff,
                          m.type, child, depth + 1);
                /* 成员相对累计: 修正 addr(嵌套结构体按累计偏移) */
                out.members.append(child);
            }
        }
        break;
    }
    case CType::Array: {
        out.type = t->elem ? mapType(t->elem) : VariableDef::Uint32;
        out.size = uint32_t(t->size);
        out.isStruct = true;       /* 可展开元素 */
        out.isArray = true;
        QString tt = elemDisplayName(t->elem);
        for (auto d : t->dims) tt += QStringLiteral("[%1]").arg(d);
        out.typeText = tt;
        buildArrayEls(name, addr + uint32_t(byteOff), t, out.members, 0);
        break;
    }
    default:
        out.type = VariableDef::Uint32;
        out.size = uint32_t(t->size ? t->size : 4);
        break;
    }
}

} // namespace

bool DwarfParser::parse(const QByteArray& elf, QVector<DwarfVar>& out, QString* err) {
    bool is64 = elf.size() > 4 && (unsigned char)elf[4] == 2;
    QMap<QString, QByteArray> secs;
    if (!getSections(elf, is64, secs)) {
        if (err) *err = "ELF section 表解析失败";
        return false;
    }
    if (!secs.contains(".debug_info")) { if (err) *err = "无调试信息(.debug_info)"; return false; }

    Ctx c;
    c.secs = &secs;
    {
        auto it = secs.constFind(QStringLiteral(".debug_addr"));
        if (it != secs.constEnd()) c.debugAddr = &it.value();
    }
    if (!parseInfo(c, err)) return false;

    /* 遍历顶层 variable DIE */
    for (Die* die : c.allDies) {
        if (die->tag != TAG_variable) continue;
        if (die->attrs.contains(AT_declaration)) continue;
        if (die->parent && die->parent->tag == TAG_compile_unit)
            {}   /* 全局变量 */
        else if (die->parent && die->parent->tag != TAG_namespace)
            continue;   /* 函数内变量暂不处理(有固定地址的 static 后续支持) */

        /* location: DW_OP_addr 或 DWARF5 的 DW_OP_addrx */
        auto loc = die->attrs.find(AT_location);
        if (loc == die->attrs.end() || loc->kind != Val::Bytes) continue;
        uint64_t addr = 0;
        {
            Rd e(reinterpret_cast<const uint8_t*>(loc->b.constData()), loc->b.size());
            bool ok = false;
            while (!e.eof()) {
                const uint8_t op = e.u8();
                if (op == OP_addr) {
                    addr = c.addrSize == 8 ? e.u64() : e.u32();
                    ok = true;
                    break;
                }
                if (op == OP_addrx || op == OP_GNU_addr_index) {
                    const uint64_t idx = e.uleb();
                    const uint64_t off = c.addrBase + idx * c.addrSize;
                    if (c.debugAddr && int(off) + int(c.addrSize) <= c.debugAddr->size()) {
                        memcpy(&addr, c.debugAddr->constData() + off, c.addrSize);
                        ok = true;
                    }
                    break;
                }
            }
            if (!ok) continue;
        }
        /* name/type: 支持 specification/abstract_origin 间接 */
        Die* src = die;
        QString name;
        Val* ty = nullptr;
        for (int hop = 0; hop < 4 && src; ++hop) {
            if (name.isEmpty()) {
                auto n = src->attrs.find(AT_name);
                if (n != src->attrs.end() && n->kind == Val::Str) name = n->s;
            }
            if (!ty) {
                auto t2 = src->attrs.find(AT_type);
                if (t2 != src->attrs.end() && t2->kind == Val::Ref) ty = &*t2;
            }
            if (!name.isEmpty() && ty) break;
            auto sp = src->attrs.find(AT_specification);
            if (sp == src->attrs.end()) sp = src->attrs.find(AT_abstract_origin);
            if (sp == src->attrs.end() || sp->kind != Val::Ref) break;
            src = c.byOffset.value(uint32_t(sp->u));
        }
        if (name.isEmpty() || !ty) continue;
        Die* typeDie = c.byOffset.value(uint32_t(ty->u));
        if (!typeDie) continue;

        CType* ct = resolveType(typeDie, c);
        DwarfVar var;
        buildNode(name, uint32_t(addr), 0, ct, var, 0);
        if (var.name.isEmpty()) continue;
        /* 所属源文件: 用编译单元的 DW_AT_name(通常为相对路径, 如 ../src/main.c);
         * 不拼接 comp_dir, 避免显示超长绝对路径 */
        {
            Die* cu = die;
            while (cu && cu->tag != TAG_compile_unit) cu = cu->parent;
            if (cu) {
                const auto fn = cu->attrs.constFind(AT_name);
                if (fn != cu->attrs.constEnd() && fn->kind == Val::Str)
                    var.file = fn->s;
            }
        }
        out.append(var);
    }

    /* 清理类型缓存 */
    return !out.isEmpty();
}
