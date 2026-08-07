/* Cancellation: SIGINT handler and child-process-group termination. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "agent.h"
#include "message.h"
#include "../http.h"
#include "../json.h"
#include "../webfetch.h"
#include "../websearch.h"
#include "../sandbox.h"
#include "../models.h"
#include "../tools/tools.h"
#include "../permissions/permissions.h"
#include "../markdown.h"
#include "../platform/platform.h"
#include "../../vendor/jsmn/jsmn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fnmatch.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>
#include <regex.h>

#include "agent_internal.h"


/* Cancellation state. Volatile sig_atomic_t because SIGINT writes them from a
 * signal handler; the agent loop reads them via ccode_cancel_pending(). */
static volatile sig_atomic_t ccode_cancel_flag = 0;
static volatile sig_atomic_t ccode_active_child = 0;
static volatile sig_atomic_t ccode_cancel_defaulted = 0;

void ccode_cancel_signal_handler(int signo) {
    (void)signo;
    ccode_cancel_flag = 1;
    /* terminate any active command process group. -1 PGID = 0 means no child. */
    if (ccode_active_child > 0) {
        kill(-(pid_t)ccode_active_child, SIGTERM);
        kill(-(pid_t)ccode_active_child, SIGKILL);
        ccode_active_child = 0;
    }
    /* A second interrupt restores the default handler so the user can force
     * kill an agent that did not wind down after the first cancellation. */
    if (ccode_cancel_defaulted) {
        signal(SIGINT, SIG_DFL);
    } else {
        ccode_cancel_defaulted = 1;
    }
}

void ccode_cancel_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = ccode_cancel_signal_handler;
    sa.sa_flags = SA_RESTART;
    (void)sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGINT, &sa, NULL);
    ccode_cancel_flag = 0;
    ccode_active_child = 0;
    ccode_cancel_defaulted = 0;
}

int ccode_cancel_pending(void) {
    return ccode_cancel_flag != 0;
}

void ccode_cancel_child_register(pid_t child) {
    ccode_active_child = (sig_atomic_t)child;
}

void ccode_cancel_child_unregister(void) {
    ccode_active_child = 0;
}
