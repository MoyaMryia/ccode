/* Robust file-descriptor I/O helpers shared by the CLI, the TUI protocol
 * and the tool-result archiver. */

#include "fdio.h"

#include <errno.h>
#include <unistd.h>
//BLAME: 那我之前那个BLAME有道理啊，你这里return -1 有消费者吗
//BLAME-IMPACT(fdio): fdio.c:8 — 返回码消费者见 protocol.c/agent_results.c；main.c 需处理
int ccode_fd_write_all(int fd, const void *data, size_t length) {
    const char *p = data;
    while (length > 0) {
        ssize_t written = write(fd, p, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        p += written;
        length -= (size_t)written;
    }
    return 0;
}
