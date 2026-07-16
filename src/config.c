#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

void ccode_print_usage(const char *program) {
    fprintf(stderr,
        "Usage: %s [options] [-p PROMPT | --interactive]\n"
        "\n"
        "Options:\n"
        "  -p, --prompt TEXT      Send one prompt then exit\n"
        "  -i, --interactive      Read prompts from stdin until EOF or /exit\n"
        "      --tui              Run the ANSI terminal UI\n"
        "      --backend PATH     Backend executable for --tui\n"
        "      --json              Use JSON Lines protocol (ccode-cli)\n"
        "      --save-session P    Save conversation to PATH after each prompt\n"
        "      --resume P          Load conversation from PATH before the run\n"
        "      --api-base URL     OpenAI-compatible API base URL\n"
        "      --api-key KEY      API key (defaults to CCODE_API_KEY)\n"
        "      --model NAME       Model name (defaults to CCODE_MODEL)\n"
        "      --read-only        Enable read-only tools (read, glob, grep)\n"
        "      --write            Enable read and write_file tools with confirmation\n"
        "      --auto-approve     Auto-approve all tool requests\n"
        "      --session-dir DIR  Session storage directory\n"
        "  -h, --help             Show this help\n"
        "\n"
        "Environment:\n"
        "  CCODE_SESSION_DIR          Session storage directory\n"
        "  CCODE_SESSION_AUTO_SAVE    Auto-save session after each turn (default: 1)\n"
        "  CCODE_SESSION_MAX_SIZE     Max session file size (default: 10M)\n"
        "  CCODE_SESSION_KEEP_COUNT   Max sessions to keep (default: 10)\n"
        "\n"
        "REPL slash commands (interactive mode):\n"
        "  /help        Show available slash commands\n"
        "  /exit        Exit the REPL\n"
        "  /clear       Forget in-conversation history for this session\n"
        "  /history     Print prompts entered this session\n"
        "  /sessions    List all saved sessions\n"
        "  /resume NAME Resume a saved session\n",
        program);
}

int ccode_parse_args(int argc, char **argv, struct ccode_config *config) {
    int i;

    memset(config, 0, sizeof(*config));
    config->api_base = getenv("CCODE_API_BASE");
    {
        const char *key = getenv("CCODE_API_KEY");
        const char *key_file = getenv("CCODE_API_KEY_FILE");
        if (!key && key_file) {
            int key_fd = open(key_file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            struct stat key_st;
            FILE *kf = NULL;
            if (key_fd >= 0 && fstat(key_fd, &key_st) == 0 &&
                S_ISREG(key_st.st_mode) && key_st.st_nlink == 1 &&
                (key_st.st_mode & 0077) == 0)
                kf = fdopen(key_fd, "rb");
            else if (key_fd >= 0)
                close(key_fd);
            if (kf) {
                static char file_buf[4096];
                size_t pos = 0;
                int c;
                int too_long = 0;
                while ((c = fgetc(kf)) != EOF) {
                    if (c == '\n' || c == '\r') continue;
                    if (pos >= sizeof(file_buf) - 1) { too_long = 1; break; }
                    file_buf[pos++] = (char)c;
                }
                fclose(kf);
                file_buf[pos] = '\0';
                if (!too_long && pos > 0) key = file_buf;
            }
        }
        config->api_key = key;
    }
    config->model = getenv("CCODE_MODEL");

    {
        const char *ro = getenv("CCODE_READ_ONLY_TOOLS");
        config->read_only_tools = (ro && ro[0] == '1') ? 1 : 0;
        ro = getenv("CCODE_WRITE_TOOLS");
        config->tools_enabled = (ro && ro[0] == '1') ? 1 : 0;
    }
    {
        const char *aa = getenv("CCODE_AUTO_APPROVE");
        config->auto_approve = (aa && aa[0] == '1') ? 1 : 0;
    }
    {
        const char *sa = getenv("CCODE_SESSION_AUTO_SAVE");
        config->session_auto_save = (!sa || sa[0] == '1') ? 1 : 0;
    }
    {
        const char *sm = getenv("CCODE_SESSION_MAX_SIZE");
        config->session_max_size = sm ? atol(sm) : 10 * 1024 * 1024;
        if (config->session_max_size <= 0)
            config->session_max_size = 10 * 1024 * 1024;
    }
    {
        const char *sk = getenv("CCODE_SESSION_KEEP_COUNT");
        config->session_keep_count = sk ? atoi(sk) : 10;
        if (config->session_keep_count <= 0)
            config->session_keep_count = 10;
    }
    config->session_dir = getenv("CCODE_SESSION_DIR");

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            ccode_print_usage(argv[0]);
            return 1;
        }
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--prompt") == 0) && i + 1 < argc) {
            config->prompt = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--api-base") == 0 && i + 1 < argc) {
            config->api_base = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--api-key") == 0 && i + 1 < argc) {
            config->api_key = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            config->model = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--read-only") == 0) {
            config->read_only_tools = 1;
            continue;
        }
        if (strcmp(argv[i], "--write") == 0) {
            config->tools_enabled = 1;
            continue;
        }
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0) {
            config->interactive = 1;
            continue;
        }
        if (strcmp(argv[i], "--tui") == 0) {
            config->interactive = 1;
            config->tui = 1;
            continue;
        }
        if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            config->backend = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--json") == 0) {
            config->json = 1;
            config->interactive = 1;
            continue;
        }
        if (strcmp(argv[i], "--auto-approve") == 0) {
            config->auto_approve = 1;
            continue;
        }
        if (strcmp(argv[i], "--save-session") == 0 && i + 1 < argc) {
            config->save_session = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--resume") == 0 && i + 1 < argc) {
            config->resume_session = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--session-dir") == 0 && i + 1 < argc) {
            config->session_dir = argv[++i];
            continue;
        }

        fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
        ccode_print_usage(argv[0]);
        return -1;
    }

    if (!config->api_base || !config->api_key || !config->model) {
        fprintf(stderr, "CCODE_API_BASE, CCODE_API_KEY, CCODE_MODEL are required.\n");
        return -1;
    }
    if (!config->prompt && !config->interactive) {
        fprintf(stderr, "Either -p PROMPT or --interactive is required.\n");
        return -1;
    }
    return 0;
}
