// <stdatomic.h> for the runtime's C headers in C++ (the .mm UI, as on the PC with clang's own header): the SDK's
// libc++ before C++23 passes it on to FreeBSD's C header, which names C's _Bool. clang's _Atomic works in C++, as
// it does on the PC; only the type name is missing. In C this is just the SDK's header.
#ifdef __cplusplus
#ifndef _Bool
#define _Bool bool
#define WSWORD_STDATOMIC_BOOL
#endif
#endif
#include_next <stdatomic.h>
#ifdef WSWORD_STDATOMIC_BOOL
#undef _Bool
#undef WSWORD_STDATOMIC_BOOL
#endif
