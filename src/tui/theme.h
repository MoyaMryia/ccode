#ifndef CCODE_TUI_THEME_H
#define CCODE_TUI_THEME_H

#define TUI_RESET "\033[0m"
#define TUI_BOLD "\033[1m"
#define TUI_DIM "\033[2m"
#define TUI_ORANGE "\033[38;2;232;133;89m"
#define TUI_CYAN "\033[36m"
#define TUI_YELLOW "\033[33m"
#define TUI_RED "\033[31m"

const char *tui_prompt_for_input(const char *input);

#endif
