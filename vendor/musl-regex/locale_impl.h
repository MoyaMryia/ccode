/* Minimal locale_impl.h stub for building musl's regex/fnmatch outside musl
 * (ccode Win32 port). musl uses this for gettext translation of regerror
 * strings and MB_CUR_MAX; we provide neither translation nor a C-library
 * locale system, so the identity mapping plus <stdlib.h> suffices. */
#ifndef CCODE_MUSL_LOCALE_IMPL_STUB_H
#define CCODE_MUSL_LOCALE_IMPL_STUB_H

#include <stdlib.h>

#define LCTRANS_CUR(s) (s)

#endif
