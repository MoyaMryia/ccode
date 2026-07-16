#include "config.h"
#include "tui/tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    struct ccode_config config;
    struct ccode_agent_config agent_cfg;
    int result;

    result = ccode_parse_args(argc, argv, &config);
    if (result != 0) return result < 0 ? 2 : 0;

    memset(&agent_cfg, 0, sizeof(agent_cfg));
    agent_cfg.api_base = config.api_base;
    agent_cfg.api_key = config.api_key;
    agent_cfg.model = config.model;
    agent_cfg.prompt = config.prompt;
    agent_cfg.tools_enabled = config.tools_enabled;
    agent_cfg.read_only_tools = config.read_only_tools;
    agent_cfg.interactive = config.interactive;
    agent_cfg.auto_approve = config.auto_approve;
    agent_cfg.save_session = config.save_session;
    agent_cfg.resume_session = config.resume_session;
    agent_cfg.workspace = getenv("CCODE_WORKSPACE");
    if (!agent_cfg.workspace) agent_cfg.workspace = ".";

    if (!config.tui) {
        fprintf(stderr, "ccode is a TUI shell; use ccode-cli for CLI mode\n");
        return 2;
    }
    return ccode_tui_run(&agent_cfg, config.backend, argc, argv);
}
