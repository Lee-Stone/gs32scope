#include "elf_parser.h"
#include <QFile>
#include <cstring>

/*
 * 最小 ELF 读取器: 只依赖文件结构本身。
 * 提取 SHT_SYMTAB 中 STT_OBJECT 且有地址的符号。
 * 过滤: "__" 前缀(编译器内部)、size=0、UNDEF/ABS 段。
 */

namespace {

constexpr int SHT_SYMTAB = 2;
constexpr int STT_OBJECT = 1;
constexpr uint16_t SHN_UNDEF = 0;
constexpr uint16_t SHN_ABS   = 0xFFF1;

/* 过滤规则, 与需求文档 4.5 一致 */
bool keep(const QString& name, uint32_t size, uint16_t shndx) {
    if (size == 0 || name.isEmpty()) return false;
    if (shndx == SHN_UNDEF || shndx == SHN_ABS) return false;
    if (name.startsWith("__")) return false;
    return true;
}

/* ---------- ELF32 ---------- */
struct Elf32_Ehdr { unsigned char e_ident[16]; uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; };
struct Elf32_Shdr { uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset,
    sh_size; uint32_t sh_link, sh_info, sh_addralign, sh_entsize; };
struct Elf32_Sym  { uint32_t st_name, st_value, st_size; uint8_t st_info,
    st_other; uint16_t st_shndx; };

/* ---------- ELF64 ---------- */
struct Elf64_Ehdr { unsigned char e_ident[16]; uint16_t e_type, e_machine;
    uint32_t e_version; uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; };
struct Elf64_Shdr { uint32_t sh_name, sh_type; uint64_t sh_flags, sh_addr,
    sh_offset, sh_size; uint32_t sh_link, sh_info; uint64_t sh_addralign,
    sh_entsize; };
struct Elf64_Sym  { uint32_t st_name; uint8_t st_info, st_other;
    uint16_t st_shndx; uint64_t st_value, st_size; };

template <typename T>
const T* readAt(const QByteArray& d, qint64 off, QString* err) {
    if (off < 0 || off + (qint64)sizeof(T) > d.size()) {
        if (err) *err = QStringLiteral("ELF 文件越界读取 @%1").arg(off);
        return nullptr;
    }
    return reinterpret_cast<const T*>(d.constData() + off);
}

bool parseCommon(const QByteArray& d, bool is64, QVector<ElfSymbol>& out,
                 QString* err) {
    uint64_t shoff, shentsize; uint16_t shnum;
    if (is64) {
        auto* eh = readAt<Elf64_Ehdr>(d, 0, err);
        if (!eh) return false;
        shoff = eh->e_shoff; shentsize = eh->e_shentsize; shnum = eh->e_shnum;
    } else {
        auto* eh = readAt<Elf32_Ehdr>(d, 0, err);
        if (!eh) return false;
        shoff = eh->e_shoff; shentsize = eh->e_shentsize; shnum = eh->e_shnum;
    }
    if (shentsize == 0 || shnum == 0) {
        if (err) *err = QStringLiteral("无 section 表(可能已 strip)");
        return false;
    }

    /* 找 .symtab */
    int symIdx = -1;
    for (uint16_t i = 0; i < shnum; ++i) {
        const qint64 off = (qint64)shoff + (qint64)i * shentsize;
        if (is64) {
            auto* sh = readAt<Elf64_Shdr>(d, off, err); if (!sh) return false;
            if (sh->sh_type == SHT_SYMTAB) { symIdx = i; break; }
        } else {
            auto* sh = readAt<Elf32_Shdr>(d, off, err); if (!sh) return false;
            if (sh->sh_type == SHT_SYMTAB) { symIdx = i; break; }
        }
    }
    if (symIdx < 0) {
        if (err) *err = QStringLiteral("未找到 .symtab (请确认编译带 -g 且未 strip)");
        return false;
    }

    const qint64 symOff = (qint64)shoff + (qint64)symIdx * shentsize;
    uint64_t symOffF, symSizeF, strOffF, strSizeF; uint64_t symEnt;
    if (is64) {
        auto* sh = readAt<Elf64_Shdr>(d, symOff, err); if (!sh) return false;
        symOffF = sh->sh_offset; symSizeF = sh->sh_size; symEnt = sh->sh_entsize;
        auto* strSh = readAt<Elf64_Shdr>(
            d, (qint64)shoff + (qint64)sh->sh_link * shentsize, err);
        if (!strSh) return false;
        strOffF = strSh->sh_offset; strSizeF = strSh->sh_size;
    } else {
        auto* sh = readAt<Elf32_Shdr>(d, symOff, err); if (!sh) return false;
        symOffF = sh->sh_offset; symSizeF = sh->sh_size; symEnt = sh->sh_entsize;
        auto* strSh = readAt<Elf32_Shdr>(
            d, (qint64)shoff + (qint64)sh->sh_link * shentsize, err);
        if (!strSh) return false;
        strOffF = strSh->sh_offset; strSizeF = strSh->sh_size;
    }
    if (symEnt == 0) symEnt = is64 ? 24 : 16;

    const int n = int(symSizeF / symEnt);
    out.reserve(n);
    for (int i = 1; i < n; ++i) {           /* [0] 为空符号 */
        const qint64 off = (qint64)symOffF + (qint64)i * symEnt;
        uint32_t st_name = 0, type = 0; uint16_t shndx = 0;
        uint64_t value = 0, size = 0;
        if (is64) {
            auto* sy = readAt<Elf64_Sym>(d, off, err); if (!sy) return false;
            st_name = sy->st_name; type = sy->st_info & 0xF;
            shndx = sy->st_shndx; value = sy->st_value; size = sy->st_size;
        } else {
            auto* sy = readAt<Elf32_Sym>(d, off, err); if (!sy) return false;
            st_name = sy->st_name; type = sy->st_info & 0xF;
            shndx = sy->st_shndx; value = sy->st_value; size = sy->st_size;
        }
        if (type != STT_OBJECT) continue;

        /* 取名字符串 */
        if ((qint64)strOffF + st_name >= (qint64)(strOffF + strSizeF)) continue;
        const char* raw = d.constData() + strOffF + st_name;
        const int maxLen = int(strSizeF - st_name);
        const QString name = QString::fromUtf8(raw, strnlen(raw, maxLen));
        if (!keep(name, (uint32_t)size, shndx)) continue;

        ElfSymbol s;
        s.name = name;
        s.addr = (uint32_t)value;
        s.size = (uint32_t)size;
        out.append(s);
    }
    return true;
}

} // namespace

bool ElfParser::parse(const QString& path, QVector<ElfSymbol>& out, QString* err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("无法打开文件: %1").arg(path);
        return false;
    }
    const QByteArray d = f.readAll();
    f.close();

    if (d.size() < 16 || qstrncmp(d.constData(), "\x7f" "ELF", 4) != 0) {
        if (err) *err = QStringLiteral("不是有效的 ELF 文件: %1").arg(path);
        return false;
    }
    const unsigned char ei_class = (unsigned char)d[4];   /* 1=32 2=64 */
    const unsigned char ei_data  = (unsigned char)d[5];   /* 1=LE */
    if (ei_data != 1) {
        if (err) *err = QStringLiteral("暂不支持大端 ELF");
        return false;
    }
    const bool ok = (ei_class == 2) ? parseCommon(d, true, out, err)
                                    : parseCommon(d, false, out, err);
    return ok;
}
