#ifndef CCODE_COMPAT_POLL_H
#define CCODE_COMPAT_POLL_H
/* libc5 ships poll() under <sys/poll.h>. Forward there so source files
 * that #include <poll.h> compile unchanged on BasicLinux. On glibc this
 * shim is not on the include path (only retro builds add compat/). */
#include <sys/poll.h>
#endif
