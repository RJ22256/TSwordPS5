// (TSword PS5) What the runtime takes from the Windows / Linux headers that the console's (FreeBSD's) lack; included
// via runtime/platform.h on the console only. strlcpy, strlcat and arc4random are the console's own.
#pragma once
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

static inline int _chsize_s(int fd, int64_t size) { return ftruncate(fd, (off_t)size); }

static inline void *_aligned_malloc(size_t size, size_t alignment) {
    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment < sizeof(void *) ? sizeof(void *) : alignment, size) != 0) return NULL;
    return ptr;
}
static inline void _aligned_free(void *ptr) { free(ptr); }
