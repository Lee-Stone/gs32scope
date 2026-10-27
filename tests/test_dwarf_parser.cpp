/*
 * DwarfParser 自测: 内存构造最小 DWARF4 ELF32(小端),
 * 验证: 变量提取 / 结构体成员展开 / 位域(小端 bit_offset 换算) / 类型映射。
 * 构建目录运行: build\tests\test_dwarf_parser.exe
 */
#include "../app/symbols/dwarf_parser.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct Buf {
    std::vector<uint8_t> v;
    void u8(uint8_t b) { v.push_back(b); }
    void u16(uint16_t b) { v.push_back(b & 0xff); v.push_back(b >> 8); }
    void u32(uint32_t b) { for (int i = 0; i < 4; ++i) v.push_back((b >> (8 * i)) & 0xff); }
    void uleb(uint64_t b) { do { uint8_t c = b & 0x7f; b >>= 7; if (b) c |= 0x80; u8(c); } while (b); }
    void bytes(const char* s, int n) { v.insert(v.end(), s, s + n); }
    void str(const char* s) { bytes(s, int(strlen(s)) + 1); }
    int  size() const { return int(v.size()); }
};

QByteArray buildTestElf() {
    /* ---------- .debug_str ---------- */
    Buf str;
    auto addStr = [&](const char* s) { int off = str.size(); str.str(s); return off; };
    str.bytes("\0", 1);
    const int offFloat = addStr("float");
    const int offUint  = addStr("unsigned int");
    const int offUchar = addStr("unsigned char");
    const int offMotor = addStr("Motor");
    const int offSpeed = addStr("speed");
    const int offFlags = addStr("flags");
    const int offMode  = addStr("mode");
    const int offTag   = addStr("tag");
    const int offMotorVar = addStr("g_motor");

    /* ---------- .debug_abbrev ---------- */
    Buf ab;
    auto abbrev = [&](uint64_t code, uint64_t tag, uint8_t children,
                      std::initializer_list<std::pair<uint64_t,uint64_t>> attrs) {
        ab.uleb(code); ab.uleb(tag); ab.u8(children);
        for (auto& a : attrs) { ab.uleb(a.first); ab.uleb(a.second); }
        ab.uleb(0); ab.uleb(0);
    };
    abbrev(1, 0x34, 0, {{0x03,0x0e},{0x49,0x13},{0x02,0x18}});   /* variable */
    abbrev(2, 0x13, 1, {{0x03,0x0e},{0x0b,0x0b}});               /* structure */
    abbrev(3, 0x0d, 0, {{0x03,0x0e},{0x49,0x13},{0x38,0x0b}});   /* member */
    abbrev(4, 0x0d, 0, {{0x03,0x0e},{0x49,0x13},{0x38,0x0b},
                        {0x0d,0x0b},{0x0c,0x0b}});               /* bitfield member */
    abbrev(5, 0x24, 0, {{0x03,0x0e},{0x0b,0x0b},{0x3e,0x0b}});   /* base_type */
    ab.uleb(0);

    /* ---------- .debug_info ---------- */
    Buf info;
    info.u32(0);              /* length 占位 */
    info.u16(4);              /* version 4 */
    info.u32(0);              /* abbrev offset */
    info.u8(4);               /* address size */
    const int dieStart = info.size();
    auto baseType = [&](uint32_t nameOff, uint8_t size, uint8_t enc) {
        info.uleb(5); info.u32(nameOff); info.u8(size); info.u8(enc);
    };
    baseType(offFloat, 4, 4);   /* float  */
    const int offUintDie = info.size();
    baseType(offUint, 4, 7);    /* unsigned int */
    const int offUcharDie = info.size();
    baseType(offUchar, 1, 8);   /* unsigned char */

    const int offStruct = info.size();
    info.uleb(2); info.u32(offMotor); info.u8(9);    /* Motor, size 9 */
    info.uleb(3); info.u32(offSpeed); info.u32(dieStart + 0); info.u8(0);      /* speed @0  */
    info.uleb(3); info.u32(offFlags); info.u32(offUintDie); info.u8(4);        /* flags @4 */
    info.uleb(4); info.u32(offMode); info.u32(offUintDie); info.u8(4);
    info.u8(3); info.u8(28);                                                   /* mode:3 @4 bit_offset=28 */
    info.uleb(3); info.u32(offTag); info.u32(offUcharDie); info.u8(8);         /* tag @8   */
    info.u8(0);   /* 子列表结束 */

    const int offVar = info.size();
    info.uleb(1); info.u32(offMotorVar); info.u32(offStruct);      /* g_motor */
    info.u8(5); info.u8(0x03);                            /* exprloc len5: DW_OP_addr */
    info.u32(0x20000000);
    info.u8(0);   /* 根列表结束 */

    const int infoEnd = info.size();
    info.v[0] = infoEnd - 4 & 0xff; info.v[1] = (infoEnd - 4 >> 8) & 0xff;
    info.v[2] = (infoEnd - 4 >> 16) & 0xff; info.v[3] = (infoEnd - 4 >> 24) & 0xff;

    /* ---------- ELF 封装 ---------- */
    const char shstr[] = "\0.debug_info\0.debug_abbrev\0.debug_str\0.shstrtab\0";
    /* 偏移: 1, 14, 29, 41, 总长 51 */
    const int ehsize = 52, shnum = 5, shstrndx = 4;
    const int shoff = ehsize;
    const int dataOff = ehsize + shnum * 40;
    const int infoOff = dataOff, abbrOff = infoOff + info.size(),
              strOff = abbrOff + ab.size(), shstrOff = strOff + str.size();

    auto* eh = new uint8_t[dataOff + info.size() + ab.size() + str.size() + sizeof(shstr)];
    uint8_t* p = eh;
    auto w32 = [&p](uint32_t v) { for (int i = 0; i < 4; ++i) *p++ = (v >> (8 * i)) & 0xff; };
    auto w16 = [&p](uint16_t v) { *p++ = v & 0xff; *p++ = v >> 8; };

    /* ELF header */
    const char magic[8] = {0x7f,'E','L','F',1,1,1,0};
    memcpy(p, magic, 8); p += 16;
    w16(2); w16(40);                 /* e_type=EXEC, e_machine=ARM */
    w32(1);                          /* e_version */
    w32(0); w32(0); w32(shoff);      /* entry, phoff, shoff */
    w32(0);                          /* flags */
    w16(ehsize); w16(0); w16(0);     /* ehsize, phentsize, phnum */
    w16(40); w16(shnum); w16(shstrndx);
    /* section headers */
    auto shdr = [&](uint32_t name, uint32_t type, uint32_t off, uint32_t size) {
        w32(name); w32(type); w32(0); w32(0); w32(off); w32(size);
        w32(0); w32(0); w32(4); w32(0);
    };
    shdr(0, 0, 0, 0);                       /* NULL */
    shdr(1, 1, infoOff, info.size());        /* .debug_info */
    shdr(13, 1, abbrOff, ab.size());         /* .debug_abbrev */
    shdr(27, 1, strOff, str.size());         /* .debug_str */
    shdr(38, 3, shstrOff, sizeof(shstr));    /* .shstrtab */
    /* data */
    memcpy(p, info.v.data(), info.size()); p += info.size();
    memcpy(p, ab.v.data(), ab.size()); p += ab.size();
    memcpy(p, str.v.data(), str.size()); p += str.size();
    memcpy(p, shstr, sizeof(shstr)); p += sizeof(shstr);

    QByteArray out(reinterpret_cast<const char*>(eh), int(p - eh));
    delete[] eh;
    return out;
}

int fails = 0;
void expect(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

bool findMember(const QVector<DwarfVar>& members, const char* shortName,
                DwarfVar& out) {
    for (const auto& m : members)
        if (m.name.endsWith("." + QString(shortName))) { out = m; return true; }
    return false;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    const QString path = dir.path() + "/t.elf";
    { QFile f(path); f.open(QIODevice::WriteOnly); f.write(buildTestElf()); }

    QFile f(path);
    f.open(QIODevice::ReadOnly);
    const QByteArray elf = f.readAll();
    f.close();

    QVector<DwarfVar> vars;
    QString err;
    const bool ok = DwarfParser::parse(elf, vars, &err);
    expect(ok, "DWARF 解析成功");
    if (!ok) { std::printf("  err: %s\n", err.toUtf8().constData()); return 1; }

    expect(vars.size() == 1, "提取 1 个全局变量");
    if (vars.isEmpty()) return 1;
    const DwarfVar& g = vars[0];
    expect(g.name == "g_motor" && g.addr == 0x20000000, "g_motor @0x20000000");
    expect(g.isStruct && g.size == 9, "g_motor 为结构体 size=9");
    expect(g.members.size() == 4, "展开 4 个成员");

    DwarfVar m;
    expect(findMember(g.members, "speed", m) && m.type == VariableDef::Float &&
           m.addr == 0x20000000, "speed: float @0x20000000");
    expect(findMember(g.members, "flags", m) && m.type == VariableDef::Uint32 &&
           m.addr == 0x20000004, "flags: uint32 @0x20000004");
    expect(findMember(g.members, "mode", m) && m.bitWidth == 3 &&
           m.bitOffset == 1 && m.addr == 0x20000004, "mode: 位域 [3:1] @0x20000004");
    expect(findMember(g.members, "tag", m) && m.type == VariableDef::Uint8 &&
           m.addr == 0x20000008, "tag: uint8 @0x20000008");

    std::printf(fails ? "== FAILED %d ==\n" : "== ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
