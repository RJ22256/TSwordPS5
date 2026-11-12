// test stand-in: the console's protection calls on Linux, with a shadow of the GPU bits per 16 KiB page
#pragma once
#include <stdint.h>
#include <stddef.h>
#define PS5_KERNEL_PROT_CPU_READ 0x01
#define PS5_KERNEL_PROT_CPU_WRITE 0x02
#define PS5_KERNEL_PROT_CPU_EXEC 0x04
#define PS5_KERNEL_PROT_GPU_READ 0x10
#define PS5_KERNEL_PROT_GPU_WRITE 0x20
int32_t sceKernelMprotect(const void *address, size_t length, int protection);
int32_t sceKernelQueryMemoryProtection(void *address, void **start, void **end, uint32_t *protection);
