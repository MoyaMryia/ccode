/*
 * Regression tests for the shell-command classifier in
 * src/security/sandbox.c.
 *
 * This is an isolated test file: the Makefile is intentionally not touched,
 * so it compiles the classifier into its own translation unit and can be
 * built and run directly:
 *
 *     cc -std=c99 -Wall -Wextra -Wpedantic -O2 \
 *        tests/test_command_classification.c -o /tmp/test_command_classification
 *     /tmp/test_command_classification
 *
 * The cases pin the security fixes made after the classification audit:
 *
 *   - destructive `rm` forms with literal TABs, several options, or
 *     redundant separators ("//etc", "/etc/./") stay hard-REFUSEd;
 *   - dangerous commands cannot hide behind command wrappers
 *     (timeout/setsid/su/flock/xargs/shells/...);
 *   - attached option paths ("tar -C/tmp", "cp -t/tmp", "-C..") cannot
 *     bypass workspace confinement;
 *   - "/etc//shadow" and "/etc/./shadow" cannot downgrade a sensitive
 *     path from TIER2;
 *   - ambiguous attached values fail closed (a prompt, never a silent
 *     workspace approval).
 */

#include "../src/security/sandbox.c"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failed;

static const char *class_name(enum ccode_command_class cls) {
    switch (cls) {
    case CCODE_CMD_ALLOW:    return "ALLOW";
    case CCODE_CMD_TIER1:    return "TIER1";
    case CCODE_CMD_TIER2:    return "TIER2";
    case CCODE_CMD_TIER3:    return "TIER3";
    case CCODE_CMD_ESCALATE: return "ESCALATE";
    case CCODE_CMD_REFUSE:   return "REFUSE";
    }
    return "?";
}

static void expect_class(const char *cmd, const char *ws,
                         enum ccode_command_class want) {
    char why[256];
    enum ccode_command_class got;
    why[0] = '\0';
    got = ccode_command_classify(cmd, ws, why, sizeof(why));
    g_checks++;
    if (got != want) {
        fprintf(stderr, "FAIL: %-46s ws=%s\n      want %s, got %s (%s)\n",
                cmd, ws ? ws : "(null)", class_name(want), class_name(got),
                why);
        g_failed++;
        return;
    }
    /* Every non-ALLOW class must explain itself. */
    if (want != CCODE_CMD_ALLOW && why[0] == '\0') {
        fprintf(stderr, "FAIL: %-46s classified %s without a reason\n",
                cmd, class_name(want));
        g_failed++;
    }
}

/* A dangerous command must be in one of the two never-approvable classes so
 * that command_policy_refuse()/command_argv_refuse() stop it outright. */
static void expect_hard_refused(const char *cmd, const char *ws) {
    char why[256];
    enum ccode_command_class got;
    why[0] = '\0';
    got = ccode_command_classify(cmd, ws, why, sizeof(why));
    g_checks++;
    if (got != CCODE_CMD_REFUSE && got != CCODE_CMD_ESCALATE) {
        fprintf(stderr, "FAIL: %-46s must never execute, got %s (%s)\n",
                cmd, class_name(got), why);
        g_failed++;
    }
}

static void test_rm_destructive_forms(void) {
    const char *ws = "/home/bob/proj";

    expect_class("rm -rf /", ws, CCODE_CMD_REFUSE);
    expect_class("rm -rf /*", ws, CCODE_CMD_REFUSE);
    expect_class("rm -rf /etc", ws, CCODE_CMD_REFUSE);
    /* Literal TABs used to slip past the space-only scan. */
    expect_class("rm\t-rf\t/", ws, CCODE_CMD_REFUSE);
    expect_class("rm\t-rf\t/etc", ws, CCODE_CMD_REFUSE);
    /* Several options used to defeat the single-option scan. */
    expect_class("rm -r -f /", ws, CCODE_CMD_REFUSE);
    expect_class("rm --preserve-root -rf /", ws, CCODE_CMD_REFUSE);
    /* Redundant separators and dot components. */
    expect_class("rm -rf //etc", ws, CCODE_CMD_REFUSE);
    expect_class("rm -rf /etc/./", ws, CCODE_CMD_REFUSE);
    /* Destructive account-file rewrites. */
    expect_class("rm /etc//shadow", ws, CCODE_CMD_REFUSE);
    expect_class("rm /etc/./shadow", ws, CCODE_CMD_REFUSE);
    expect_class("find / -delete", ws, CCODE_CMD_REFUSE);
    expect_class("echo c > /proc/sysrq-trigger", ws, CCODE_CMD_REFUSE);
    /* A relative "." is the workspace, not the filesystem root. */
    expect_class("rm -rf .", ws, CCODE_CMD_ALLOW);
}

static void test_wrappers_do_not_hide_danger(void) {
    const char *ws = "/home/bob/proj";

    /* Power commands behind wrappers must reach TIER3. */
    expect_class("timeout 10 reboot", ws, CCODE_CMD_TIER3);
    expect_class("timeout -k 5 10 reboot", ws, CCODE_CMD_TIER3);
    expect_class("timeout --signal=KILL 10 reboot", ws, CCODE_CMD_TIER3);
    expect_class("setsid reboot", ws, CCODE_CMD_TIER3);
    expect_class("setsid systemctl reboot", ws, CCODE_CMD_TIER3);
    expect_class("sh -c 'systemctl reboot'", ws, CCODE_CMD_TIER3);
    expect_class("bash -c 'systemctl poweroff'", ws, CCODE_CMD_TIER3);
    expect_class("busybox sh -c reboot", ws, CCODE_CMD_TIER3);
    expect_class("sudo -u root reboot", ws, CCODE_CMD_TIER3);
    expect_class("nice -n 10 reboot", ws, CCODE_CMD_TIER3);
    expect_class("env -u FOO reboot", ws, CCODE_CMD_TIER3);
    expect_class("su -c reboot", ws, CCODE_CMD_TIER3);
    expect_class("runuser -c reboot", ws, CCODE_CMD_TIER3);
    expect_class("timeout 5 shutdown -h now", ws, CCODE_CMD_TIER3);
    expect_class("flock /tmp/l reboot", ws, CCODE_CMD_TIER3);
    expect_class("xargs reboot", ws, CCODE_CMD_TIER3);
    expect_class("watch -n 1 reboot", ws, CCODE_CMD_TIER3);
    expect_class("chroot /mnt reboot", ws, CCODE_CMD_TIER3);
    /* Shell command strings attached directly to -c. */
    expect_class("sh -creboot", ws, CCODE_CMD_TIER3);
    expect_class("bash -csystemctl reboot", ws, CCODE_CMD_TIER3);

    /* Escalate-class tooling behind wrappers must stay hard-refused. */
    expect_hard_refused("timeout 5 grub-install --target=x86_64-efi", ws);
    expect_hard_refused("setsid wipefs --all", ws);
    expect_hard_refused("nice -n 10 cryptsetup luksFormat", ws);
    expect_hard_refused("sh -c 'mknod /tmp/x b 1 3'", ws);
    expect_hard_refused("timeout 5 mknod /tmp/x b 1 3", ws);
    expect_hard_refused("sudo -u root fdisk -l", ws);
    expect_hard_refused("sh -c 'rm -rf /etc'", ws);
    expect_hard_refused("timeout 5 rm /etc//shadow", ws);
    expect_hard_refused("timeout 5 dd if=/dev/zero of=/etc/shadow", ws);
}

static void test_sensitive_path_obfuscation(void) {
    const char *ws = "/home/bob/proj";

    /* Redundant separators / dot components must not downgrade TIER2. */
    expect_class("cat /etc//shadow", ws, CCODE_CMD_TIER2);
    expect_class("cat /etc/./shadow", ws, CCODE_CMD_TIER2);
    expect_class("cat /etc//gshadow", ws, CCODE_CMD_TIER2);
    expect_class("cat /etc/./sudoers", ws, CCODE_CMD_TIER2);
    expect_class("cat /etc//ssh/ssh_host_rsa_key", ws, CCODE_CMD_TIER2);
    expect_class("cat /proc/self//environ", ws, CCODE_CMD_TIER2);
    expect_class("cat /etc/../etc/shadow", ws, CCODE_CMD_TIER2);
}

static void test_attached_option_paths(void) {
    const char *ws = "/home/bob/proj";

    /* Attached option values used to look like workspace-relative files. */
    expect_class("tar -C/tmp/outside -xf x.tar", ws, CCODE_CMD_TIER1);
    expect_class("cp -t/tmp/outside a b", ws, CCODE_CMD_TIER1);
    expect_class("tar -C../outside -xf x.tar", ws, CCODE_CMD_TIER1);
    expect_class("cp -t../outside a b", ws, CCODE_CMD_TIER1);
    expect_class("tar -C.. -xf x.tar", ws, CCODE_CMD_TIER1);
    expect_class("gcc -I/usr/include -c x.c", ws, CCODE_CMD_TIER1);
    /* Ambiguous attached linker value: fail closed, not auto-approve. */
    expect_class("gcc -Wl,-rpath,/tmp -o x x.c", ws, CCODE_CMD_TIER1);

    /* Genuinely workspace-confined attached values still auto-approve. */
    expect_class("tar -Cbuild -xf x.tar", ws, CCODE_CMD_ALLOW);
    expect_class("cp -t./out a b", ws, CCODE_CMD_ALLOW);
    expect_class("gcc -I./src -c x.c", ws, CCODE_CMD_ALLOW);
    expect_class("tar -C /home/bob/proj/build -xf x.tar", ws, CCODE_CMD_ALLOW);
}

static void test_regression_baselines(void) {
    const char *ws = "/home/bob/proj";
    static const char *const dangerous[] = {
        "mkfs.ext4 /dev/sda",
        "dd if=/dev/zero of=/dev/sda",
        "fsck /dev/sda1",
        "rm -rf /",
        "rm -rf /etc",
        "echo c > /proc/sysrq-trigger",
    };
    size_t i;

    for (i = 0; i < sizeof(dangerous) / sizeof(dangerous[0]); i++)
        expect_hard_refused(dangerous[i], ws);

    expect_class("make -j4", ws, CCODE_CMD_ALLOW);
    expect_class("gcc -o build/out src/main.c", ws, CCODE_CMD_ALLOW);
    expect_class("cat /home/bob/proj/x", ws, CCODE_CMD_ALLOW);
    expect_class("cat /etc/shadow", ws, CCODE_CMD_TIER2);
    expect_class("sudo make install", ws, CCODE_CMD_TIER2);
    expect_class("reboot", ws, CCODE_CMD_TIER3);
}

int main(void) {
    test_rm_destructive_forms();
    test_wrappers_do_not_hide_danger();
    test_sensitive_path_obfuscation();
    test_attached_option_paths();
    test_regression_baselines();

    printf("command classification tests: %d checks, %d failure(s)\n",
           g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
