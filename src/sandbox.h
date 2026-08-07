#ifndef CCODE_SANDBOX_H
#define CCODE_SANDBOX_H

#include "platform/platform.h"

/* Command-level path filtering: refuse commands that reference sensitive
 * paths (ssh keys, cloud credentials, shadow, etc.). Returns 1 when the
 * command must be refused, 0 when it is allowed. */
/* Command-level mitigation. `workspace` is the absolute workspace root
 * (NULL = none): soft-sensitive patterns (home dirs, /.config/) are
 * tolerated when the referenced path is inside the workspace. Hard
 * patterns (credentials, /etc/shadow, key material) are always refused. */
int ccode_command_is_sensitive(const char *text, const char *workspace);

/* Refuse destructive commands (mkfs, dd, chown, ...) appearing as a word in
 * the command text. Returns 1 when the command must be refused. */
int ccode_command_mentions_destructive(const char *text);

#endif
