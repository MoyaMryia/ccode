/*
 * Combined single-binary entry point.
 *
 * `ccode` is a single executable that carries both the TUI frontend and the
 * CLI backend in one process (no fork/exec of a separate ccode-cli). It
 * dispatches on how it was invoked:
 *
 *   - bare `ccode`                  -> in-process TUI (agent runs in this process)
 *   - `ccode -p "..."` / `-i`       -> CLI single-prompt / interactive REPL
 *   - `ccode --json`                -> JSON Lines backend (for other frontends)
 *   - `ccode-cli` (symlink/rename)  -> CLI backend
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ccode_tui_main_inprocess(int argc, char **argv);
int ccode_cli_main(int argc, char **argv);

static int is_cli_invocation(int argc, char **argv) {
    const char *prog;
    const char *slash;
    int i;
    if (argc < 1 || !argv[0]) return 0;
    prog = argv[0];
    slash = strrchr(prog, '/');
    if (slash) prog = slash + 1;
    /* Renamed/symlinked as *-cli (e.g. ccode-cli). */
    if (strlen(prog) >= 4 && strcmp(prog + strlen(prog) - 4, "-cli") == 0)
        return 1;
    /* Explicit CLI modes. */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--prompt") == 0 ||
            strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0 ||
            strcmp(argv[i], "--json") == 0)
            return 1;
    }
    return 0;
}

#ifdef _WIN32
/*
 * Native Windows build: the TUI runs through the console-API renderer
 * (win32_console.c) instead of termios/ANSI. Bare `ccode` starts the
 * in-process TUI when attached to a console; when stdin/stdout are
 * redirected (pipes, files) it falls back to the line-based REPL so the
 * binary stays scriptable. Explicit CLI flags work unchanged.
 */
int main(int argc, char **argv) {
    if (is_cli_invocation(argc, argv)) return ccode_cli_main(argc, argv);
    if (isatty(0) && isatty(1)) {
        int i;
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--no-tui") == 0) break;
        }
        if (i >= argc) return ccode_tui_main_inprocess(argc, argv);
    }
    {
        int i;
        char **repl_argv = malloc(sizeof(char *) * (size_t)(argc + 2));
        if (!repl_argv) return 1;
        for (i = 0; i < argc; i++) {
            repl_argv[i] = argv[i];
            if (strcmp(argv[i], "--tui") == 0) {
                if (isatty(0) && isatty(1)) {
                    free(repl_argv);
                    return ccode_tui_main_inprocess(argc, argv);
                }
                fprintf(stderr, "--tui requires a console; falling back to "
                                "the interactive REPL.\n");
            }
        }
        repl_argv[argc] = "--interactive";
        repl_argv[argc + 1] = NULL;
        i = ccode_cli_main(argc + 1, repl_argv);
        free(repl_argv);
        return i;
    }
}
#else
int main(int argc, char **argv) {
    if (is_cli_invocation(argc, argv)) return ccode_cli_main(argc, argv);
    return ccode_tui_main_inprocess(argc, argv);
}
#endif /* _WIN32 */
