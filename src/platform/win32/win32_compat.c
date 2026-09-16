/*
 * Native Win32 (MinGW) compatibility layer — implementations.
 *
 * Two design pillars:
 *
 *  1. Virtual directory fds. agent_fs.c walks the workspace with
 *     openat()/fstatat()/renameat()/... on directory fds, a POSIX API with
 *     no CRT equivalent. We emulate it: opening a directory returns a
 *     "virtual fd" (an index into a path table, numbered high above any CRT
 *     fd), and all *at() functions resolve their dirfd through the table.
 *     Regular files still get real CRT fds, so fdopen()/fread() keep
 *     working untouched.
 *
 *  2. Socket detection by probing. Winsock SOCKETs are opaque handles whose
 *     numeric range overlaps CRT fds. read()/write()/close() therefore probe
 *     with ioctlsocket(FIONREAD): success means the fd is a socket and the
 *     call is routed to recv/send/closesocket, with WSA error codes mapped
 *     onto errno values so the existing EINTR/EAGAIN logic keeps working.
 *
 * Baseline: Windows XP SP2+ (_WIN32_WINNT=0x0501). No Vista+ APIs.
 */

#include "win32_compat.h"

#ifdef _WIN32

#include <stdarg.h>
#include <poll.h> /* shim: struct pollfd + POLL* constants */

/* The implementations below must call the native CRT/Winsock functions, not
 * their shadowed selves: drop every macro the force-included header made. */
#undef open
#undef openat
#undef close
#undef read
#undef write
#undef dup
#undef fstat
#undef fstatat
#undef unlinkat
#undef renameat
#undef fdopendir
#undef fchdir
#undef fsync
#undef fchmod
#undef fchown
#undef realpath
#undef fcntl
#undef mkdir
#undef rename
#undef setenv
#undef unsetenv
#undef clock_gettime
#undef nanosleep
#undef strcasestr
#undef send
#undef recv
#undef connect
#undef kill
#undef inet_pton
#undef lstat
#undef poll

/* ── Virtual directory fd table ── */

#define CCODE_VFD_BASE 16384
#define CCODE_VFD_MAX  256

static char *ccode_vfd_paths[CCODE_VFD_MAX];

static int vfd_alloc(const char *path) {
    int i;
    for (i = 0; i < CCODE_VFD_MAX; i++) {
        if (!ccode_vfd_paths[i]) {
            size_t len = strlen(path);
            ccode_vfd_paths[i] = malloc(len + 1);
            if (!ccode_vfd_paths[i]) return -1;
            memcpy(ccode_vfd_paths[i], path, len + 1);
            return CCODE_VFD_BASE + i;
        }
    }
    errno = EMFILE;
    return -1;
}

static int is_vfd(int fd) {
    return fd >= CCODE_VFD_BASE && fd < CCODE_VFD_BASE + CCODE_VFD_MAX;
}

static const char *vfd_path(int fd) {
    if (!is_vfd(fd)) return NULL;
    return ccode_vfd_paths[fd - CCODE_VFD_BASE];
}

static void vfd_free(int fd) {
    if (is_vfd(fd)) {
        free(ccode_vfd_paths[fd - CCODE_VFD_BASE]);
        ccode_vfd_paths[fd - CCODE_VFD_BASE] = NULL;
    }
}

/* Join a dirfd (virtual or AT_FDCWD) with a relative name into a full path.
 * Absolute paths (C:\..., C:/..., \..., /...) pass through unchanged.
 * Forward slashes are used throughout; the CRT and Win32 both accept them. */
static int vfd_join(int dirfd, const char *name, char *out, size_t cap) {
    const char *base;
    size_t bl, nl;
    int n;

    if (!name || !out || cap < 8) return -1;
    if (name[0] == '/' || name[0] == '\\' ||
        (name[0] && name[1] == ':')) {
        n = snprintf(out, cap, "%s", name);
        return (n > 0 && (size_t)n < cap) ? 0 : -1;
    }
    if (dirfd == AT_FDCWD) {
        n = snprintf(out, cap, "%s", name);
        return (n > 0 && (size_t)n < cap) ? 0 : -1;
    }
    base = vfd_path(dirfd);
    if (!base) { errno = EBADF; return -1; }
    bl = strlen(base);
    nl = strlen(name);
    if (bl + 1 + nl + 1 > cap) { errno = ENAMETOOLONG; return -1; }
    memcpy(out, base, bl);
    out[bl] = '/';
    memcpy(out + bl + 1, name, nl + 1);
    return 0;
}

/* Is this fd a Winsock socket? Probe: ioctlsocket fails with WSAENOTSOCK
 * for CRT fds and virtual fds. */
static int fd_is_socket(int fd) {
    unsigned long dummy;
    if (is_vfd(fd)) return 0;
    return ioctlsocket((SOCKET)(uintptr_t)fd, FIONREAD, &dummy) == 0;
}

/* ── WSA -> errno mapping ── */

int ccode_win32_map_wsa_error(int wsa_err) {
    int e;
    switch (wsa_err) {
    case 0:                 return 0;
    case WSAEINTR:          e = EINTR; break;
    case WSAEWOULDBLOCK:    e = EAGAIN; break;
    case WSAEINPROGRESS:    e = EINPROGRESS; break;
    case WSAEALREADY:       e = EALREADY; break;
    case WSAENOTSOCK:       e = EBADF; break;
    case WSAEMSGSIZE:       e = EMSGSIZE; break;
    case WSAEINVAL:         e = EINVAL; break;
    case WSAEAFNOSUPPORT:   e = EAFNOSUPPORT; break;
    case WSAEADDRINUSE:     e = EADDRINUSE; break;
    case WSAEADDRNOTAVAIL:  e = EADDRNOTAVAIL; break;
    case WSAECONNREFUSED:   e = ECONNREFUSED; break;
    case WSAECONNRESET:     e = ECONNRESET; break;
    case WSAECONNABORTED:   e = ECONNABORTED; break;
    case WSAETIMEDOUT:      e = ETIMEDOUT; break;
    case WSAENETUNREACH:    e = ENETUNREACH; break;
    case WSAEHOSTUNREACH:   e = EHOSTUNREACH; break;
    case WSAENOTCONN:       e = ENOTCONN; break;
    case WSAEISCONN:        e = EISCONN; break;
    case WSAEMFILE:         e = EMFILE; break;
    default:                e = EIO; break;
    }
    errno = e;
    return e;
}

/* ── Winsock init + console setup (constructor) ── */

static int ccode_ansi_state = -1; /* -1 unknown, 0 no, 1 yes */

static void ccode_win32_console_init(void) {
    HANDLE out;
    DWORD mode;
    ccode_ansi_state = 0;
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE &&
        GetConsoleMode(out, &mode)) {
        /* ENABLE_VIRTUAL_TERMINAL_PROCESSING (0x4) exists on Win10+ only;
         * on XP/7 SetConsoleMode fails and we stay in plain-text mode. */
        if (SetConsoleMode(out, mode | 0x0004 | 0x0008))
            ccode_ansi_state = 1;
    }
}

__attribute__((constructor))
static void ccode_win32_runtime_init(void) {
    WSADATA wsa;
    (void)WSAStartup(MAKEWORD(2, 2), &wsa);
    ccode_win32_console_init();
    /* Binary mode for stdin/stdout: the agent protocol and tool IO assume
     * byte streams, not text-mode CRLF translation. */
    (void)_setmode(0, O_BINARY);
    (void)_setmode(1, O_BINARY);
}

int ccode_win32_ansi_ok(void) {
    if (ccode_ansi_state < 0) ccode_win32_console_init();
    return ccode_ansi_state;
}

/* ── open / openat ── */

static int path_is_directory(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES &&
           (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static int ccode_win32_open_impl(const char *path, int flags, mode_t mode) {
    int crt_flags;
    int fd;

    if (!path || !path[0]) { errno = ENOENT; return -1; }

    if (flags & O_DIRECTORY) {
        if (!path_is_directory(path)) { errno = ENOTDIR; return -1; }
        return vfd_alloc(path);
    }
    /* Opening a directory without O_DIRECTORY: still hand out a virtual fd
     * (POSIX allows O_RDONLY on a dir; the CRT does not). */
    if (path_is_directory(path))
        return vfd_alloc(path);

    /* Strip flags unknown to the CRT; force binary mode (the agent deals in
     * byte streams and computes digests/offsets itself). */
    crt_flags = (flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND |
                          O_CREAT | O_TRUNC | O_EXCL)) | O_BINARY;
    fd = _open(path, crt_flags, mode ? (int)mode : 0666);
    return fd;
}

int ccode_win32_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    va_list ap;
    if (flags & O_CREAT) {
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return ccode_win32_open_impl(path, flags, mode);
}

int ccode_win32_openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    char full[4096];
    va_list ap;
    if (flags & O_CREAT) {
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (vfd_join(dirfd, path, full, sizeof(full)) != 0) return -1;
    return ccode_win32_open_impl(full, flags, mode);
}

/* ── close / read / write / dup ── */

int ccode_win32_close(int fd) {
    if (is_vfd(fd)) {
        vfd_free(fd);
        return 0;
    }
    if (fd_is_socket(fd)) {
        if (closesocket((SOCKET)(uintptr_t)fd) != 0) {
            ccode_win32_map_wsa_error(WSAGetLastError());
            return -1;
        }
        return 0;
    }
    return _close(fd);
}

ssize_t ccode_win32_read(int fd, void *buf, size_t n) {
    int got;
    if (is_vfd(fd)) { errno = EISDIR; return -1; }
    if (fd_is_socket(fd)) {
        got = recv((SOCKET)(uintptr_t)fd, (char *)buf, (int)n, 0);
        if (got == SOCKET_ERROR) {
            ccode_win32_map_wsa_error(WSAGetLastError());
            return -1;
        }
        return (ssize_t)got;
    }
    return (ssize_t)_read(fd, buf, (unsigned int)n);
}

ssize_t ccode_win32_write(int fd, const void *buf, size_t n) {
    int sent;
    if (is_vfd(fd)) { errno = EISDIR; return -1; }
    if (fd_is_socket(fd)) {
        sent = send((SOCKET)(uintptr_t)fd, (const char *)buf, (int)n, 0);
        if (sent == SOCKET_ERROR) {
            ccode_win32_map_wsa_error(WSAGetLastError());
            return -1;
        }
        return (ssize_t)sent;
    }
    return (ssize_t)_write(fd, buf, (unsigned int)n);
}

int ccode_win32_send(int fd, const void *buf, size_t len, int flags) {
    int sent;
    (void)flags; /* MSG_NOSIGNAL is meaningless on Windows */
    sent = send((SOCKET)(uintptr_t)fd, (const char *)buf, (int)len, 0);
    if (sent == SOCKET_ERROR) {
        ccode_win32_map_wsa_error(WSAGetLastError());
        return -1;
    }
    return sent;
}

int ccode_win32_recv(int fd, void *buf, size_t len, int flags) {
    int got;
    (void)flags;
    got = recv((SOCKET)(uintptr_t)fd, (char *)buf, (int)len, 0);
    if (got == SOCKET_ERROR) {
        ccode_win32_map_wsa_error(WSAGetLastError());
        return -1;
    }
    return got;
}

int ccode_win32_connect(int fd, const struct sockaddr *addr, int addrlen) {
    if (connect((SOCKET)(uintptr_t)fd, addr, addrlen) != 0) {
        int err = WSAGetLastError();
        /* Classic winsock divergence: a nonblocking connect reports
         * WSAEWOULDBLOCK (10035), while the POSIX callers check for
         * EINPROGRESS. Normalize all three "in progress" spellings. */
        if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS ||
            err == WSAEALREADY) {
            errno = EINPROGRESS;
            return -1;
        }
        ccode_win32_map_wsa_error(err);
        return -1;
    }
    return 0;
}

int ccode_win32_dup(int fd) {
    if (is_vfd(fd)) {
        const char *p = vfd_path(fd);
        char copy[4096];
        if (!p) { errno = EBADF; return -1; }
        if (strlen(p) >= sizeof(copy)) { errno = ENAMETOOLONG; return -1; }
        memcpy(copy, p, strlen(p) + 1);
        return vfd_alloc(copy);
    }
    return _dup(fd);
}

/* ── stat family ── */

/* msvcrt stat() mishandles trailing slashes and bare drive letters; the
 * workspace walk only produces clean paths, but normalize defensively. */
static int ccode_stat_path(const char *path, struct stat *st) {
    char copy[4096];
    size_t len;
    int ret;

    if (!path || strlen(path) >= sizeof(copy)) { errno = ENAMETOOLONG; return -1; }
    memcpy(copy, path, strlen(path) + 1);
    len = strlen(copy);
    while (len > 3 && (copy[len - 1] == '/' || copy[len - 1] == '\\'))
        copy[--len] = '\0';
    ret = stat(copy, st);
    if (ret == 0 && path_is_directory(copy))
        st->st_mode = (st->st_mode & ~S_IFMT) | S_IFDIR;
    return ret;
}

int ccode_win32_fstat(int fd, struct stat *st) {
    const char *p;
    if (is_vfd(fd)) {
        p = vfd_path(fd);
        if (!p) { errno = EBADF; return -1; }
        return ccode_stat_path(p, st);
    }
    if (fd_is_socket(fd)) { errno = EBADF; return -1; }
    /* Plain CRT fstat (macro/inline onto _fstat64i32): matches struct stat. */
    return fstat(fd, st);
}

int ccode_win32_fstatat(int dirfd, const char *path, struct stat *st, int flags) {
    char full[4096];
    (void)flags; /* AT_SYMLINK_NOFOLLOW: no symlink semantics in v1 */
    if (vfd_join(dirfd, path, full, sizeof(full)) != 0) return -1;
    return ccode_stat_path(full, st);
}

/* ── unlinkat / renameat / rename / mkdir ── */

int ccode_win32_unlinkat(int dirfd, const char *path, int flags) {
    char full[4096];
    if (vfd_join(dirfd, path, full, sizeof(full)) != 0) return -1;
    if (flags & 0x200 /* AT_REMOVEDIR */)
        return _rmdir(full);
    if (_unlink(full) != 0) {
        if (path_is_directory(full)) return _rmdir(full);
        return -1;
    }
    return 0;
}

int ccode_win32_rename(const char *oldp, const char *newp) {
    /* CRT rename() refuses to replace; POSIX semantics require atomic
     * replace. MoveFileEx with REPLACE_EXISTING covers files on the same
     * volume (the atomic-write temp file lives in the same directory). */
    if (MoveFileExA(oldp, newp,
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return 0;
    errno = EACCES;
    if (GetLastError() == ERROR_FILE_NOT_FOUND ||
        GetLastError() == ERROR_PATH_NOT_FOUND)
        errno = ENOENT;
    return -1;
}

int ccode_win32_renameat(int oldfd, const char *oldp, int newfd, const char *newp) {
    char ofull[4096];
    char nfull[4096];
    if (vfd_join(oldfd, oldp, ofull, sizeof(ofull)) != 0) return -1;
    if (vfd_join(newfd, newp, nfull, sizeof(nfull)) != 0) return -1;
    return ccode_win32_rename(ofull, nfull);
}

int ccode_win32_mkdir(const char *path, mode_t mode) {
    char copy[4096];
    size_t len;
    size_t i;
    (void)mode;

    if (!path || strlen(path) >= sizeof(copy)) { errno = ENAMETOOLONG; return -1; }
    memcpy(copy, path, strlen(path) + 1);
    len = strlen(copy);
    /* mkdir -p semantics: session dirs are nested ($HOME/.ccode/sessions). */
    for (i = 1; i < len; i++) {
        if (copy[i] == '/' || copy[i] == '\\') {
            if (copy[i - 1] == ':') continue; /* drive root */
            copy[i] = '\0';
            if (!path_is_directory(copy)) (void)_mkdir(copy);
            copy[i] = '/';
        }
    }
    if (_mkdir(copy) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* ── directory streams ── */

DIR *ccode_win32_fdopendir(int fd) {
    const char *p;
    char copy[4096];
    DIR *d;

    p = vfd_path(fd);
    if (!p) { errno = EBADF; return NULL; }
    if (strlen(p) >= sizeof(copy)) { errno = ENAMETOOLONG; return NULL; }
    memcpy(copy, p, strlen(p) + 1);
    /* fdopendir consumes the fd: closedir() must not see it again. */
    vfd_free(fd);
    d = opendir(copy);
    return d;
}

int ccode_win32_fchdir(int fd) {
    const char *p = vfd_path(fd);
    if (!p) { errno = EBADF; return -1; }
    return _chdir(p);
}

/* ── durability / ownership stubs ── */

int ccode_win32_fsync(int fd) {
    if (is_vfd(fd)) return 0; /* no directory fsync on Windows */
    return _commit(fd);
}

int ccode_win32_fchmod(int fd, mode_t mode) {
    (void)fd; (void)mode;
    return 0;
}

int ccode_win32_fchown(int fd, uid_t uid, gid_t gid) {
    (void)fd; (void)uid; (void)gid;
    return 0;
}

/* ── realpath ── */

char *ccode_win32_realpath(const char *path, char *resolved) {
    static char buf[4096];
    char *out;
    DWORD n;
    size_t i, len;

    out = resolved ? resolved : buf;
    n = GetFullPathNameA(path, 4096, out, NULL);
    if (n == 0 || n >= 4096) return NULL;
    len = strlen(out);
    for (i = 0; i < len; i++)
        if (out[i] == '\\') out[i] = '/';
    while (len > 3 && out[len - 1] == '/')
        out[--len] = '\0';
    return out;
}

/* ── fcntl ── */

int ccode_win32_fcntl(int fd, int cmd, ...) {
    va_list ap;
    long arg = 0;
    unsigned long on;

    va_start(ap, cmd);
    if (cmd == F_SETFL || cmd == F_SETFD) arg = va_arg(ap, long);
    va_end(ap);

    switch (cmd) {
    case F_SETFD:
        return 0; /* CRT fds are non-inheritable by default */
    case F_GETFL:
        return 0;
    case F_SETFL:
        if (fd_is_socket(fd)) {
            on = (arg & O_NONBLOCK) ? 1UL : 0UL;
            if (ioctlsocket((SOCKET)(uintptr_t)fd, FIONBIO, &on) != 0) {
                ccode_win32_map_wsa_error(WSAGetLastError());
                return -1;
            }
        }
        return 0;
    default:
        return 0;
    }
}

/* ── environment ── */

int ccode_win32_setenv(const char *name, const char *value, int overwrite) {
    size_t nl, vl;
    char *entry;

    if (!name || !value) { errno = EINVAL; return -1; }
    if (!overwrite && getenv(name)) return 0;
    nl = strlen(name);
    vl = strlen(value);
    entry = malloc(nl + 1 + vl + 1);
    if (!entry) { errno = ENOMEM; return -1; }
    memcpy(entry, name, nl);
    entry[nl] = '=';
    memcpy(entry + nl + 1, value, vl + 1);
    /* _putenv keeps the pointer; intentional leak, same as glibc setenv. */
    return _putenv(entry);
}

int ccode_win32_unsetenv(const char *name) {
    size_t nl;
    char *entry;
    if (!name) { errno = EINVAL; return -1; }
    nl = strlen(name);
    entry = malloc(nl + 2);
    if (!entry) { errno = ENOMEM; return -1; }
    memcpy(entry, name, nl);
    entry[nl] = '=';
    entry[nl + 1] = '\0';
    return _putenv(entry);
}

/* ── time ── */

int ccode_win32_clock_gettime(int clk, struct timespec *ts) {
    static LARGE_INTEGER freq;
    static int freq_ok = 0;
    LARGE_INTEGER now;
    FILETIME ft;
    unsigned __int64 t64;

    if (!ts) { errno = EINVAL; return -1; }
    if (clk == CLOCK_MONOTONIC) {
        if (!freq_ok) {
            if (!QueryPerformanceFrequency(&freq)) { errno = EINVAL; return -1; }
            freq_ok = 1;
        }
        if (!QueryPerformanceCounter(&now)) { errno = EIO; return -1; }
        ts->tv_sec = (time_t)(now.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)((now.QuadPart % freq.QuadPart) *
                             1000000000LL / freq.QuadPart);
        return 0;
    }
    GetSystemTimeAsFileTime(&ft);
    t64 = ((unsigned __int64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t64 -= 116444736000000000ULL; /* 1601 -> 1970 */
    ts->tv_sec = (time_t)(t64 / 10000000ULL);
    ts->tv_nsec = (long)((t64 % 10000000ULL) * 100);
    return 0;
}

int ccode_win32_nanosleep(const struct timespec *req, struct timespec *rem) {
    DWORD ms;
    (void)rem;
    if (!req) { errno = EINVAL; return -1; }
    ms = (DWORD)(req->tv_sec * 1000 + req->tv_nsec / 1000000);
    Sleep(ms);
    return 0;
}

/* ── strings ── */

char *ccode_win32_strcasestr(const char *haystack, const char *needle) {
    size_t nl;
    const char *p;
    if (!haystack || !needle) return NULL;
    nl = strlen(needle);
    if (nl == 0) return (char *)haystack;
    for (p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nl) == 0) return (char *)p;
    }
    return NULL;
}

/* ── process termination ── */

int ccode_win32_kill(pid_t pid, int sig) {
    HANDLE h;
    DWORD target;
    (void)sig;

    /* Negative pid: POSIX group kill. Windows has no process groups here;
     * our cancel module stores process HANDLEs directly, so this path is
     * only a best-effort fallback. */
    target = (DWORD)(pid < 0 ? -pid : pid);
    h = OpenProcess(PROCESS_TERMINATE, FALSE, target);
    if (!h) { errno = ESRCH; return -1; }
    if (!TerminateProcess(h, 1)) {
        CloseHandle(h);
        errno = EIO;
        return -1;
    }
    CloseHandle(h);
    return 0;
}

/* ── inet_pton (XP lacks it) ── */

int ccode_win32_inet_pton(int af, const char *src, void *dst) {
    if (af == AF_INET) {
        unsigned long a = inet_addr(src);
        unsigned int b0, b1, b2, b3;
        char tail;
        if (a == INADDR_NONE) return 0;
        /* inet_addr accepts shorthand forms; require dotted-quad. */
        if (sscanf(src, "%u.%u.%u.%u%c", &b0, &b1, &b2, &b3, &tail) != 4 ||
            b0 > 255 || b1 > 255 || b2 > 255 || b3 > 255)
            return 0;
        memcpy(dst, &a, 4);
        return 1;
    }
    if (af == AF_INET6) {
        /* WSAStringToAddressA exists since Winsock2 (Windows 2000/XP). */
        SOCKADDR_IN6 sa;
        INT sz = (INT)sizeof(sa);
        char copy[64];
        memset(&sa, 0, sizeof(sa));
        if (!src || strlen(src) >= sizeof(copy)) return 0;
        memcpy(copy, src, strlen(src) + 1);
        if (WSAStringToAddressA(copy, AF_INET6, NULL,
                                (LPSOCKADDR)&sa, &sz) != 0)
            return 0;
        memcpy(dst, &sa.sin6_addr, 16);
        return 1;
    }
    errno = EAFNOSUPPORT;
    return -1;
}

/* ── poll via select (sockets only; XP has no WSAPoll) ── */

int ccode_win32_poll(struct pollfd *fds, unsigned long nfds, int timeout_ms) {
    fd_set rfds, wfds, efds;
    struct timeval tv;
    struct timeval *ptv;
    unsigned long i;
    int ret;
    int count = 0;

    if (nfds == 0) {
        if (timeout_ms > 0) Sleep((DWORD)timeout_ms);
        return 0;
    }

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    for (i = 0; i < nfds; i++) {
        SOCKET s = (SOCKET)(uintptr_t)fds[i].fd;
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        if (fds[i].events & POLLIN) FD_SET(s, &rfds);
        if (fds[i].events & POLLOUT) FD_SET(s, &wfds);
        FD_SET(s, &efds);
    }
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (long)(timeout_ms % 1000) * 1000;
        ptv = &tv;
    } else {
        ptv = NULL;
    }
    ret = select(0, &rfds, &wfds, &efds, ptv);
    if (ret == SOCKET_ERROR) {
        ccode_win32_map_wsa_error(WSAGetLastError());
        return -1;
    }
    if (ret == 0) return 0;
    for (i = 0; i < nfds; i++) {
        SOCKET s = (SOCKET)(uintptr_t)fds[i].fd;
        short re = 0;
        if (fds[i].fd < 0) { fds[i].revents = POLLNVAL; count++; continue; }
        if (FD_ISSET(s, &rfds)) re |= POLLIN;
        if (FD_ISSET(s, &wfds)) re |= POLLOUT;
        if (FD_ISSET(s, &efds)) re |= POLLERR;
        /* Distinguish HUP: readable + zero bytes pending = closed. */
        if ((re & POLLIN) && fds[i].events & POLLIN) {
            unsigned long avail = 1;
            char peekbuf[1];
            int pk = recv(s, peekbuf, 1, MSG_PEEK);
            if (pk == 0) re |= POLLHUP;
            else if (pk == SOCKET_ERROR &&
                     WSAGetLastError() == WSAECONNRESET)
                re |= POLLERR | POLLHUP;
            (void)avail;
        }
        if (re) { fds[i].revents = re; count++; }
    }
    return count;
}

/* ── Default CA bundle location ──
 * Windows has no /etc/ssl/certs. Look for cacert.pem next to the exe first,
 * then in the current directory. CCODE_CA_FILE (checked by the caller
 * first) still wins. */
int ccode_win32_default_ca_file(char *buf, size_t cap) {
    char exe[4096];
    DWORD n;
    char *p;
    int len;

    if (!buf || cap < 16) return -1;
    n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
    if (n > 0 && n < (DWORD)sizeof(exe)) {
        for (p = exe; *p; p++)
            if (*p == '\\') *p = '/';
        p = strrchr(exe, '/');
        if (p) {
            *p = '\0';
            len = snprintf(buf, cap, "%s/cacert.pem", exe);
            if (len > 0 && (size_t)len < cap &&
                GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES)
                return 0;
        }
    }
    len = snprintf(buf, cap, "cacert.pem");
    if (len > 0 && (size_t)len < cap &&
        GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES)
        return 0;
    return -1;
}

/* ── HOME fallback ── */

const char *ccode_win32_home(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = getenv("USERPROFILE");
    if (!home || !home[0]) home = getenv("HOMEDRIVE");
    return home;
}

#endif /* _WIN32 */
