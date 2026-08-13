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

int main(int argc, char **argv) {
    if (is_cli_invocation(argc, argv)) return ccode_cli_main(argc, argv);
    return ccode_tui_main_inprocess(argc, argv);
}
