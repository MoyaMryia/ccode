#ifndef CCODE_TUI_H
#define CCODE_TUI_H

#include "../agent/agent.h"

int ccode_tui_run(struct ccode_agent_config *config, const char *backend_path,
                  int argc, char **argv);

/* 进程内 TUI：不 fork 后端进程，agent 直接在当前进程里运行，输出经回调
 * 渲染到终端。用于单体的 `ccode` 二进制（省一个进程）。 */
int ccode_tui_run_inprocess(struct ccode_agent_config *config, int argc,
                            char **argv);

#endif
