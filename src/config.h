#ifndef CCODE_CONFIG_H
#define CCODE_CONFIG_H

struct ccode_config {
    const char *api_base;
    const char *api_key;
    const char *model;
    const char *prompt;
    int tools_enabled;
    int read_only_tools;
    int interactive;
    int tui;
    int json;
    const char *backend;
    int auto_approve;
    int thinking_enabled;
    const char *thinking_effort;
    const char *save_session;
    const char *resume_session;
    const char *session_dir;
    int session_auto_save;
    long session_max_size;
    int session_keep_count;
    int markdown;
};

int ccode_parse_args(int argc, char **argv, struct ccode_config *config);
void ccode_print_usage(const char *program);

#endif
