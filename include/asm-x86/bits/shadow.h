
#include <asm-generic/xenomai/bits/shadow.h>

#ifdef __i386__
#include "shadow_32.h"
#else
#include "shadow_64.h"
#endif
