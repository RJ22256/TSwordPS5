// An in-process loader for the game code library where the system can't load one (the PS5: a title can't dlopen a
// module it brings - gamecode.c). The library is an ordinary x86-64 ELF shared object (TSword Setup's PS5 export
// builds it: tools/build_ps5.py); its segments go into executable memory, its relocations are applied,
// and what it imports is found by name in the table the host gives (the runtime's own functions and data, and the
// libc / libm functions its headers declare).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    void *addr;
} ElfExport;

// Executable memory for the image: bytes, readable, writable and executable (the host's own allocator; NULL: none).
typedef void *(*ElfExecAlloc)(size_t bytes);

typedef struct {
    uint8_t *base;  // where the image is
    size_t size;
    const void *dynsym;     // its dynamic symbols (Elf64_Sym), for elf_symbol
    const char *dynstr;
    size_t nsyms;
} ElfImage;

// Loads path. exports: sorted by name (strcmp), n of them. false: why says what's wrong (the first imports nothing
// exports among them).
bool elf_load(const char *path, const ElfExport *exports, size_t n, ElfExecAlloc alloc, ElfImage *out, char *why,
              size_t why_size);
// A symbol the image defines (its dynamic symbols), or NULL.
void *elf_symbol(const ElfImage *img, const char *name);

#ifdef __cplusplus
}
#endif
