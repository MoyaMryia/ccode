#ifndef CCODE_TUI_H
#define CCODE_TUI_H

#include "../agent/agent.h"

int ccode_tui_run(struct ccode_agent_config *config, const char *backend_path,
                  int argc, char **argv);

#endif
