// The game code library's in-process loader (elfload.h; from WSword PS5). Only the console builds it (gamecode.c on
// __PROSPERO__); ELFLOAD_TEST builds it on a PC to check it there. The ELF structures are spelled out here, as the
// ELF specification has them, rather than taken from a system header (the console's and Linux's name them apart).
#if defined(__PROSPERO__) || defined(ELFLOAD_TEST)
#include "elfload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Ehdr;
typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} Phdr;
typedef struct {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
} Shdr;
typedef struct {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
} Sym;
typedef struct {
    uint64_t offset, info;
    int64_t addend;
} Rela;
typedef struct {
    int64_t tag;
    uint64_t val;
} Dyn;

enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7, SHT_DYNSYM = 11, SHF_ALLOC = 2 };
enum {
    DT_NULL = 0, DT_RELA = 7, DT_RELASZ = 8, DT_RELAENT = 9, DT_INIT = 12, DT_JMPREL = 23, DT_PLTRELSZ = 2,
    DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27, DT_RELACOUNT = 0x6ffffff9,
};
enum { R_NONE = 0, R_64 = 1, R_GLOB_DAT = 6, R_JUMP_SLOT = 7, R_RELATIVE = 8, R_IRELATIVE = 37 };

static bool fail(char *why, size_t n, const char *fmt, const char *a, unsigned long long b) {
    snprintf(why, n, fmt, a, b);
    return false;
}

static void *find_export(const ElfExport *e, size_t n, const char *name) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(e[mid].name, name);
        if (!c) return e[mid].addr;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

void *elf_symbol(const ElfImage *img, const char *name) {
    const Sym *s = img->dynsym;
    for (size_t i = 1; i < img->nsyms; i++)
        if (s[i].shndx && !strcmp(img->dynstr + s[i].name, name)) return img->base + s[i].value;
    return NULL;
}

void *elf_section(const ElfImage *img, const char *name, size_t *size) {
    for (int i = 0; i < img->nsections; i++)
        if (!strcmp(img->sections[i].name, name)) {
            if (size) *size = img->sections[i].size;
            return img->sections[i].addr;
        }
    if (size) *size = 0;
    return NULL;
}

bool elf_load(const char *path, const ElfExport *exports, size_t nexports, ElfExecAlloc alloc, ElfImage *out,
              char *why, size_t wn) {
    memset(out, 0, sizeof *out);
    FILE *f = fopen(path, "rb");
    if (!f) return fail(why, wn, "can't open %s%.0llu", path, 0);
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = fsize > 0 ? malloc((size_t)fsize) : NULL;
    bool read_ok = file && fread(file, 1, (size_t)fsize, f) == (size_t)fsize;
    fclose(f);
    if (!read_ok) { free(file); return fail(why, wn, "can't read %s%.0llu", path, 0); }
    bool ok = false;
    const Ehdr *eh = (const Ehdr *)file;
    if ((size_t)fsize < sizeof *eh || memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 || eh->machine != 62 ||
        eh->type != 3 || eh->phoff + (uint64_t)eh->phnum * sizeof(Phdr) > (uint64_t)fsize) {
        fail(why, wn, "%s is not an x86-64 shared library%.0llu", path, 0);
        goto done;
    }
    const Phdr *ph = (const Phdr *)(file + eh->phoff);
    uint64_t span = 0;
    const Phdr *dynp = NULL;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_TLS) { fail(why, wn, "%s has thread-local data (not supported)%.0llu", path, 0); goto done; }
        if (ph[i].type == PT_DYNAMIC) dynp = &ph[i];
        if (ph[i].type == PT_LOAD && ph[i].vaddr + ph[i].memsz > span) span = ph[i].vaddr + ph[i].memsz;
    }
    if (!dynp || !span) { fail(why, wn, "%s has no dynamic section%.0llu", path, 0); goto done; }
    span = (span + 0xFFFF) & ~0xFFFFull;
    uint8_t *base = alloc(span);
    if (!base) { fail(why, wn, "no executable memory for the game code (%s, %llu MB)", path, span >> 20); goto done; }
    memset(base, 0, span);
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD) {
            if (ph[i].offset + ph[i].filesz > (uint64_t)fsize) { fail(why, wn, "%s is cut short%.0llu", path, 0); goto done; }
            memcpy(base + ph[i].vaddr, file + ph[i].offset, ph[i].filesz);
        }
    out->base = base, out->size = span;
    // its dynamic symbols (the section headers say how many)
    const Shdr *sh = (const Shdr *)(file + eh->shoff);
    if (eh->shoff && eh->shoff + (uint64_t)eh->shnum * sizeof(Shdr) <= (uint64_t)fsize) {
        const Shdr *names = eh->shstrndx < eh->shnum ? &sh[eh->shstrndx] : NULL;
        for (int i = 0; i < eh->shnum; i++) {
            if (sh[i].type == SHT_DYNSYM && sh[i].link < eh->shnum) {
                out->dynsym = base + sh[i].addr;
                out->nsyms = sh[i].size / sizeof(Sym);
                out->dynstr = (const char *)base + sh[sh[i].link].addr;
            }
            // the loaded (SHF_ALLOC) sections inside the image, by name
            if (names && (sh[i].flags & SHF_ALLOC) && sh[i].addr + sh[i].size <= span &&
                names->offset + sh[i].name < (uint64_t)fsize && out->nsections < 64) {
                const char *nm = (const char *)file + names->offset + sh[i].name;
                size_t len = strnlen(nm, (size_t)((uint64_t)fsize - names->offset - sh[i].name));
                if (len && len < sizeof out->sections[0].name) {
                    memcpy(out->sections[out->nsections].name, nm, len + 1);
                    out->sections[out->nsections].addr = base + sh[i].addr;
                    out->sections[out->nsections].size = sh[i].size;
                    out->nsections++;
                }
            }
        }
        }
    if (!out->dynsym) { fail(why, wn, "%s has no symbol table%.0llu", path, 0); goto done; }
    const Dyn *dyn = (const Dyn *)(base + dynp->vaddr);
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, init = 0, init_array = 0, init_arraysz = 0;
    for (; dyn->tag != DT_NULL; dyn++) switch (dyn->tag) {
        case DT_RELA: rela = dyn->val; break;
        case DT_RELASZ: relasz = dyn->val; break;
        case DT_JMPREL: jmprel = dyn->val; break;
        case DT_PLTRELSZ: pltsz = dyn->val; break;
        case DT_INIT: init = dyn->val; break;
        case DT_INIT_ARRAY: init_array = dyn->val; break;
        case DT_INIT_ARRAYSZ: init_arraysz = dyn->val; break;
        }
    const Sym *syms = out->dynsym;
    char missing[512] = "";
    unsigned nmissing = 0;
    for (int pass = 0; pass < 2; pass++) {
        const Rela *r = (const Rela *)(base + (pass ? jmprel : rela));
        size_t n = (pass ? pltsz : relasz) / sizeof(Rela);
        if (!(pass ? jmprel : rela)) continue;
        for (size_t i = 0; i < n; i++) {
            uint32_t type = (uint32_t)r[i].info, si = (uint32_t)(r[i].info >> 32);
            uint64_t *at = (uint64_t *)(base + r[i].offset);
            if (r[i].offset + 8 > span) { fail(why, wn, "%s: a relocation outside the image%.0llu", path, 0); goto done; }
            if (type == R_NONE) continue;
            if (type == R_RELATIVE) { *at = (uint64_t)(uintptr_t)base + (uint64_t)r[i].addend; continue; }
            if (type == R_IRELATIVE) {
                uint64_t (*resolver)(void) = (uint64_t(*)(void))(base + r[i].addend);
                *at = resolver();
                continue;
            }
            if (type != R_64 && type != R_GLOB_DAT && type != R_JUMP_SLOT) {
                fail(why, wn, "%s: relocation type %llu not supported", path, type);
                goto done;
            }
            if (si >= out->nsyms) { fail(why, wn, "%s: a relocation's symbol is out of range%.0llu", path, 0); goto done; }
            const Sym *s = &syms[si];
            const char *name = out->dynstr + s->name;
            void *addr = s->shndx ? base + s->value : find_export(exports, nexports, name);
            if (!addr && (s->info >> 4) == 2) addr = NULL;  // (a weak import nothing defines: null, as ld.so leaves it)
            else if (!addr) {
                if (nmissing++ < 12 && strlen(missing) + strlen(name) + 2 < sizeof missing) {
                    if (missing[0]) strcat(missing, " ");
                    strcat(missing, name);
                }
                continue;
            }
            *at = (uint64_t)(uintptr_t)addr + (type == R_64 ? (uint64_t)r[i].addend : 0);
        }
    }
    if (nmissing) {
        snprintf(why, wn, "the game code needs %u function%s TSword for the PS5 doesn't have: %s%s", nmissing,
                 nmissing == 1 ? "" : "s", missing, nmissing > 12 ? " ..." : "");
        goto done;
    }
    if (init) ((void (*)(void))(base + init))();
    for (uint64_t i = 0; init_array && i < init_arraysz / 8; i++) {
        uint64_t fn = ((uint64_t *)(base + init_array))[i];
        if (fn && fn != ~0ull) ((void (*)(void))(uintptr_t)fn)();  // (already relocated: absolute addresses)
    }
    // .ctors: the PS5 target's compiler puts constructors there (not .init_array), and ld.lld keeps the section
    // without a dynamic tag for it, so only its name finds it. Run last to first, as crtstuff does; 0 and -1 are its
    // markers. (The runtime's own constructors in the library: ov_zstd.c and mouse_direct.c register their host
    // functions on the patch page there - v0.3.5 stopped at the zstd one, "no function at guest pc ...fff0".)
    {
        size_t csize = 0;
        const uint64_t *ctors = elf_section(out, ".ctors", &csize);
        for (size_t i = csize / 8; ctors && i-- > 0;) {
            uint64_t fn = ctors[i];
            if (fn && fn != ~0ull) ((void (*)(void))(uintptr_t)fn)();
        }
    }
    ok = true;
done:
    free(file);
    return ok;
}
#endif
