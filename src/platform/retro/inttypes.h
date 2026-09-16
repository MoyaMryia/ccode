#ifndef CCODE_COMPAT_INTTYPES_H
#define CCODE_COMPAT_INTTYPES_H
/* libc5 has no <inttypes.h>; PolarSSL 1.3.9 headers include it just for
 * the exact-width typedefs, which <stdint.h> (our shim on libc5, the
 * real header on glibc) already provides. */
#include <stdint.h>
#endif
