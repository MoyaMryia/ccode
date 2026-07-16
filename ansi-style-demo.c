#include <stdio.h>

#define RESET "\033[0m"
#define BOLD "\033[1m"
#define ITALIC "\033[3m"
#define MAGENTA "\033[95m"
#define BOLD_MAGENTA "\033[1;95m"
#define BRIGHT_WHITE "\033[1;97m"
#define RGB_PINK "\033[1;38;2;255;0;180m"

static void sample(const char *label, const char *style,
                   const char *english, const char *chinese) {
    printf("%-18s %s%s / %s%s\n", label, style, english, chinese, RESET);
}

int main(void) {
    puts("ANSI style comparison / 中英文混合样式对比");
    puts("============================================================");
    sample("normal", "", "Normal Text", "普通文本");
    sample("bold", BOLD, "Bold Text", "粗体文本");
    sample("italic", ITALIC, "Italic Text", "斜体文本");
    sample("bold + magenta", BOLD_MAGENTA, "Bold Text", "粗体文本");
    sample("bold + white", BRIGHT_WHITE, "Bold Text", "粗体文本");
    sample("bold + RGB pink", RGB_PINK, "Bold Text", "粗体文本");
    sample("color only", MAGENTA, "Color Text", "彩色文本");
    puts("============================================================");
    puts("The ccode Markdown style is currently: ESC[1;95m");
    puts("Try the same program in the terminal where ccode looks wrong.");
    return 0;
}
