#include "config.h"
#include "tui/tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void tui_fill_agent(struct ccode_agent_config *agent_cfg,
                           const struct ccode_config *config) {
    memset(agent_cfg, 0, sizeof(*agent_cfg));
    agent_cfg->api_base = config->api_base;
    agent_cfg->api_key = config->api_key;
    agent_cfg->model = config->model;
    agent_cfg->prompt = config->prompt;
    agent_cfg->tools_enabled = config->tools_enabled;
    agent_cfg->read_only_tools = config->read_only_tools;
    agent_cfg->interactive = config->interactive;
    agent_cfg->auto_approve = config->auto_approve;
    agent_cfg->allow_http = config->allow_http;
    agent_cfg->thinking_enabled = config->thinking_enabled;
    agent_cfg->thinking_effort = config->thinking_effort;
    agent_cfg->save_session = config->save_session;
    agent_cfg->resume_session = config->resume_session;
    agent_cfg->workspace = getenv("CCODE_WORKSPACE");
    if (!agent_cfg->workspace) agent_cfg->workspace = ".";
}

/* 分离的 TUI 前端：fork ccode-cli 当后端，走 JSON Lines 协议。 */
int ccode_tui_main(int argc, char **argv) {
    struct ccode_config config;
    struct ccode_agent_config agent_cfg;
    int result = ccode_parse_args(argc, argv, &config);
    if (result != 0) return result < 0 ? 2 : 0;
    /* ccode-tui is always interactive, so a bare `--thinking`/`--write` must
     * not trip the CLI's "either -p or --interactive" requirement. */
    config.interactive = 1;
    tui_fill_agent(&agent_cfg, &config);
    (void)config.tui; /* --tui is accepted for compatibility, not required. */
    return ccode_tui_run(&agent_cfg, config.backend, argc, argv);
}

#ifdef CCODE_COMBINED
/* 进程内 TUI：不 fork 后端，直接在当前进程里跑 agent（省一个进程）。 */
int ccode_tui_main_inprocess(int argc, char **argv) {
    struct ccode_config config;
    struct ccode_agent_config agent_cfg;
    int result = ccode_parse_args(argc, argv, &config);
    if (result != 0) return result < 0 ? 2 : 0;
    config.interactive = 1;
    tui_fill_agent(&agent_cfg, &config);
    return ccode_tui_run_inprocess(&agent_cfg, argc, argv);
}
#else
int main(int argc, char **argv) {
    return ccode_tui_main(argc, argv);
}
#endif
