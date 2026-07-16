#ifndef CCODE_AGENT_H
#define CCODE_AGENT_H

#include <sys/types.h>
#include "message.h"

typedef void (*ccode_content_callback)(const char *content, void *context);

struct ccode_agent_config {
    const char *api_base;
    const char *api_key;
    const char *model;
    const char *prompt;
    int tools_enabled;
    int read_only_tools;
    int interactive;
    int auto_approve;
    const char *save_session;
    const char *resume_session;
    const char *workspace;
    ccode_content_callback on_content;
    void *on_content_context;
};

int ccode_agent_run(struct ccode_agent_config *cfg);
int ccode_agent_run_interactive(struct ccode_agent_config *cfg);
void ccode_print_content_delta(const char *content);

/* Markdown rendering control for human-facing output.  When disabled,
 * ccode_print_content_delta falls back to raw sanitised passthrough. */
void ccode_print_content_set_markdown(int enabled);
void ccode_print_content_flush(void);
void ccode_print_content_reset(void);

/* Default behavior contract injected when local tools are enabled. */
const char *ccode_coding_agent_system_prompt(void);

/* Cancellation: installed by agent_run via sigaction. SIGINT sets an atomic
 * cancel flag and terminates any active child process group. The next loop
 * iteration observes the flag, drains streams, and returns a structured
 * cancellation result. A second SIGINT reverts to the default disposition so
 * the user can force-kill a runaway agent. */
void ccode_cancel_install(void);
void ccode_cancel_signal_handler(int signo);
int ccode_cancel_pending(void);
void ccode_cancel_child_register(pid_t child);
void ccode_cancel_child_unregister(void);

#endif
