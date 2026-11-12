#pragma once
#include <stddef.h>
#define PS5_SHM_READ 0x1
#define PS5_SHM_WRITE 0x2
int ps5_vrange_reserve_at(void *address, size_t bytes);
int ps5_vrange_reserve(size_t bytes, void *hint, size_t alignment, void **base);
int ps5_vrange_release(void *base, size_t bytes);
int ps5_vrange_commit(void *address, size_t bytes, int protection);
int ps5_vrange_decommit(void *address, size_t bytes);
