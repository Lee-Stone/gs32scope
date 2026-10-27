/*
 * ElfParser 自测: 内存构造最小 ELF32(小端) 并验证符号过滤逻辑。
 * 覆盖: STT_OBJECT 提取 / "__" 前缀过滤 / size=0 过滤 / 字符串表读取。
 * 构建目录下运行: build\tests\test_elf_parser.exe
 */
#include "../app/symbols/elf_parser.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

#pragma pack(push, 1)
struct Elf32Ehdr {
    unsigned char e_ident[16]; uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct Elf32Shdr {
    uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info, sh_addralign, sh_entsize;
};
struct Elf32Sym {
    uint32_t st_name, st_value, st_size;
    uint8_t st_info, st_other; uint16_t st_shndx;
};
#pragma pack(pop)

QByteArray buildTestElf() {
    /* 符号表: [0]=空, 1=counter(uint32), 2=speed_f(float),
     *         3=__hidden(应过滤), 4=zero_size(应过滤) */
    const char strtab[] = "\0counter\0speed_f\0__hidden\0zero_size\0";
    std::vector<Elf32Sym> syms(5);
    syms[1] = {1,  0x20000000, 4, 0x11, 0, 1};   /* GLOBAL OBJECT, @.data */
    syms[2] = {9,  0x20000004, 4, 0x11, 0, 1};
    syms[3] = {17, 0x20000008, 4, 0x11, 0, 1};
    syms[4] = {26, 0x2000000C, 0, 0x11, 0, 1};   /* size=0 */

    const uint32_t ehsize = sizeof(Elf32Ehdr);
    const uint32_t symOff = ehsize + 3 * sizeof(Elf32Shdr);
    const uint32_t strOff = symOff + uint32_t(syms.size() * sizeof(Elf32Sym));

    Elf32Ehdr eh{};
    memcpy(eh.e_ident, "\x7f" "ELF\1\1\1", 8);
    eh.e_type = 2; eh.e_machine = 40; eh.e_version = 1;
    eh.e_shoff = ehsize; eh.e_ehsize = ehsize;
    eh.e_shentsize = sizeof(Elf32Shdr); eh.e_shnum = 3; eh.e_shstrndx = 0;

    Elf32Shdr sh[3]{};
    sh[1] = {0, 2, 0, 0, symOff, uint32_t(syms.size() * sizeof(Elf32Sym)),
             2, 1, 4, sizeof(Elf32Sym)};                 /* SHT_SYMTAB, link=2 */
    sh[2] = {0, 3, 0, 0, strOff, sizeof(strtab), 0, 0, 1, 0};  /* SHT_STRTAB */

    QByteArray d;
    d.append(reinterpret_cast<const char*>(&eh), sizeof(eh));
    d.append(reinterpret_cast<const char*>(sh), sizeof(sh));
    d.append(reinterpret_cast<const char*>(syms.data()),
             int(syms.size() * sizeof(Elf32Sym)));
    d.append(strtab, int(sizeof(strtab)));
    return d;
}

int fails = 0;
void expect(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    QTemporaryDir dir;
    const QString path = dir.path() + "/test.elf";
    { QFile f(path); f.open(QIODevice::WriteOnly); f.write(buildTestElf()); }

    QVector<ElfSymbol> syms;
    QString err;
    const bool ok = ElfParser::parse(path, syms, &err);
    expect(ok, "解析成功");
    if (ok) {
        expect(syms.size() == 2, "恰好保留 2 个符号(过滤 __/size=0)");
        bool hasCounter = false, hasSpeed = false, noHidden = true;
        for (const auto& s : syms) {
            if (s.name == "counter" && s.addr == 0x20000000 && s.size == 4) hasCounter = true;
            if (s.name == "speed_f" && s.addr == 0x20000004) hasSpeed = true;
            if (s.name.startsWith("__")) noHidden = false;
        }
        expect(hasCounter, "counter @0x20000000 size=4");
        expect(hasSpeed,   "speed_f  @0x20000004");
        expect(noHidden,   "__ 前缀已被过滤");
    } else {
        std::printf("  err: %s\n", err.toUtf8().constData());
    }

    /* 坏文件 */
    { QFile f(dir.path() + "/bad.elf"); f.open(QIODevice::WriteOnly); f.write("not an elf"); }
    QVector<ElfSymbol> tmp;
    QString err2;
    expect(!ElfParser::parse(dir.path() + "/bad.elf", tmp, &err2),
           "非 ELF 文件正确报错");

    std::printf(fails ? "== FAILED %d ==\n" : "== ALL PASS ==\n", fails);
    return fails ? 1 : 0;
}
