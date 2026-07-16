#ifndef CCODE_TOOLS_H
#define CCODE_TOOLS_H

#include <stddef.h>

struct ccode_tool_def {
    const char *name;
    const char *description;
    const char *param_schema;
};

extern const struct ccode_tool_def ccode_tool_definitions[];
extern const size_t ccode_tool_definitions_count;

char *ccode_build_tools_json(void);
char *ccode_build_readonly_tools_json(void);
char *ccode_build_write_tools_json(void);

#endif
