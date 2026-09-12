#ifndef CCODE_FDIO_H
#define CCODE_FDIO_H

#include <stddef.h>

/* Write the whole buffer to a file descriptor, retrying on EINTR.
 * Returns 0 on success, -1 on error or when the fd refuses more bytes. */
int ccode_fd_write_all(int fd, const void *data, size_t length);

#endif
