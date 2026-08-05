#ifndef CCODE_COMPAT_STDINT_H
#define CCODE_COMPAT_STDINT_H
/* libc5 has no <stdint.h>. The exact-width integer typedefs come from
 * <linux/types.h> (pulled in transitively by the socket headers), so
 * here we only re-expose what the sources expect and fill the gaps.
 * On glibc a real <stdint.h> exists; don't shadow it (SIZE_MAX etc.
 * would be lost). The -Isrc/compat path is harmless because the real
 * header wins via its own include guard once we forward to it. */
#if defined(__GLIBC__) && !defined(CCODE_RETRO_FORCE_SHIM)
#include_next <stdint.h>
#else

#ifndef UINT8_MAX
typedef unsigned char       uint8_t;
#endif
#ifndef UINT16_MAX
typedef unsigned short      uint16_t;
#endif
#ifndef UINT32_MAX
typedef unsigned int        uint32_t;
#endif
#ifndef UINT64_MAX
typedef unsigned long long  uint64_t;
#endif
#ifndef INT8_MIN
typedef signed char         int8_t;
#endif
#ifndef INT16_MIN
typedef short               int16_t;
#endif
#ifndef INT32_MIN
typedef int                 int32_t;
#endif
#ifndef INT64_MIN
typedef long long           int64_t;
#endif
#ifndef __intptr_t_defined
typedef long                intptr_t;
#define __intptr_t_defined 1
#endif
#ifndef __uintptr_t_defined
typedef unsigned long       uintptr_t;
#define __uintptr_t_defined 1
#endif

#define UINT8_MAX  0xffu
#define UINT16_MAX 0xffffu
#define UINT32_MAX 0xffffffffu
#define UINT64_MAX 0xffffffffffffffffull
#define INT8_MIN   (-0x80 - 1)
#define INT16_MIN  (-0x8000 - 1)
#define INT32_MIN  (-0x80000000 - 1)
#define INT64_MIN  (-0x8000000000000000ll - 1)
#define INT8_MAX   0x7f
#define INT16_MAX  0x7fff
#define INT32_MAX  0x7fffffff
#define INT64_MAX  0x7fffffffffffffffll
#define SIZE_MAX   ((size_t)-1)
#define INTPTR_MIN INT32_MIN
#define INTPTR_MAX INT32_MAX
#define UINTPTR_MAX UINT32_MAX

#endif /* glibc */
#endif /* CCODE_COMPAT_STDINT_H */
