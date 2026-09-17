#ifndef CCODE_SANDBOX_H
#define CCODE_SANDBOX_H

#include "../platform/platform.h"

#include <stddef.h>

/* Risk classes for a shell command, in increasing order. A classifier
 * answers "how much friction should this command face?" rather than the
 * old binary refuse/allow split:
 *
 *   ALLOW    - workspace-confined and unremarkable; no prompt.
 *   TIER1    - outside the workspace (or unplaceable); confirm with "y".
 *   TIER2    - credentials / elevated / opaque; confirm with "Yes".
 *   TIER3    - system power, fork bombs, recursive damage at /; confirm
 *              with "Yes, do as I say.".
 *   ESCALATE - raw device / filesystem / partition / boot modification:
 *              refused, and the error tells the model to hand it to the
 *              user instead of working around the refusal.
 *   REFUSE   - commands that destroy the running system outright.
 *
 * ESCALATE and REFUSE are never approvable in-band. Hard credential paths
 * map to TIER2; soft paths outside the workspace map to TIER1. */
enum ccode_command_class {
    CCODE_CMD_ALLOW = 0,
    CCODE_CMD_TIER1,
    CCODE_CMD_TIER2,
    CCODE_CMD_TIER3,
    CCODE_CMD_ESCALATE,
    CCODE_CMD_REFUSE
};

enum ccode_command_class ccode_command_classify(const char *text,
                                                const char *workspace,
                                                char *reason,
                                                size_t reason_size);

/* Best-effort static check: does every path this command mentions stay
 * inside `workspace`? Used to auto-approve a workspace-confined `bash`
 * command without a prompt. It is a mitigation, not a guarantee: anything
 * it cannot positively place inside the workspace (absolute paths outside,
 * "..", "~", backslash/colon separators, "$" expansions, command
 * substitution) makes it return 0, so obfuscation degrades to a prompt
 * rather than a silent escape. `workspace` is the absolute realpath root;
 * a NULL/empty root always returns 0. */
int ccode_command_stays_in_workspace(const char *text, const char *workspace);

#endif
