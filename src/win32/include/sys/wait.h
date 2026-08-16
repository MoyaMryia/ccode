/* sys/wait.h shim for native Win32 builds.
 *
 * No waitpid/fork on Windows: the process-execution code is rewritten with
 * CreateProcess and encodes the outcome in the same status layout these
 * macros decode (normal exit: (code & 0xff) << 8; killed: raw signal number).
 * Call sites therefore keep their POSIX result-assembly logic unchanged. */
#ifndef CCODE_WIN32_SHIM_SYS_WAIT_H
#define CCODE_WIN32_SHIM_SYS_WAIT_H

#ifndef WNOHANG
#define WNOHANG 1
#endif

#define WIFEXITED(s)    (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)  (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WIFSTOPPED(s)   0
#define WSTOPSIG(s)     0

#endif
