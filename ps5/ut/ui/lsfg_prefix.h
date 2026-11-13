// lsfg-vk on the console (ps5/wsword/configure.py: -include for its sources): FreeBSD's <sys/types.h> defines
// major() / minor() as macros, which take the names of lsfg-vk's version fields. Included first, then undefined: the
// header's guard keeps them from coming back.
#pragma once
#include <sys/types.h>
#undef major
#undef minor
