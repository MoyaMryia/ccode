#include "status.h"
#include "render.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

void tui_status_render(int cols, const char *model, const char *workspace,
                       int thinking_enabled, const char *thinking_effort) {
    tui_render_move(0, 0);
    tui_render_clear_line();
    int used = 20;
    fputs(TUI_DIM "ccode" TUI_RESET " " TUI_BOLD "v0.1" TUI_RESET
          " · " TUI_ORANGE, stdout);
    tui_render_text(model ? model : "unknown", cols - 20);
    used += model ? (int)strlen(model) : 7;
    fputs(TUI_RESET " · ", stdout);
    tui_render_text(workspace ? workspace : ".", cols - used - 3);
    if (thinking_enabled) {
        const char *eff = thinking_effort ? thinking_effort : "medium";
        int need = 12 + (int)strlen(eff);
        if (used + need < cols) {
            fputs(" · " TUI_CYAN "thinking:", stdout);
            fputs(eff, stdout);
            fputs(TUI_RESET, stdout);
            used += need;
        }
    }
}
