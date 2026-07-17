#ifndef CCODE_TUI_STATUS_H
#define CCODE_TUI_STATUS_H

void tui_status_render(int cols, const char *model, const char *workspace,
                       int thinking_enabled, const char *thinking_effort);

#endif
