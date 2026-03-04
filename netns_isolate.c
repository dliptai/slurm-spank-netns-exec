/*
 * netns_isolate.c - SPANK plugin for partition-based network namespace isolation
 *
 * PURPOSE:
 *   Automatically runs job tasks inside a pre-created network namespace when
 *   the job is submitted to a designated partition. Uses `ip netns exec` via
 *   spank_prepend_task_argv(), giving full parity with running
 *   `ip netns exec <n> <command>` manually, including:
 *     - entering the named network namespace
 *     - creating a private mount namespace
 *     - bind-mounting files from /etc/netns/<n>/ over /etc/
 *
 * CONFIGURATION:
 *   The partition name and namespace name are set via plugstack.conf arguments:
 *     optional /usr/lib/slurm/netns_isolate.so partition=isolated-jobs netns=isolated
 *
 *   Multiple partition=netns mappings are not supported — one plugin instance
 *   per partition if needed.
 *
 * PRE-REQUISITES:
 *   The namespace must be created on each compute node before use, e.g. via
 *   a systemd service:
 *
 *     ip netns add isolated
 *     ip netns exec isolated ip link set lo up
 *
 *   Per-namespace config (e.g. custom resolv.conf) can be placed in
 *   /etc/netns/<n>/ and will be bind-mounted automatically by ip.
 *
 *   Namespaces persist until the node reboots and are shared across jobs —
 *   they carry no per-job state.
 *
 * DEPLOYMENT:
 *   Build:
 *     gcc -std=c99 -shared -fPIC -Wall -Wextra -I/usr/include/slurm \
 *         -o netns_isolate.so netns_isolate.c -lslurm
 *
 *   Install:
 *     cp netns_isolate.so /usr/lib/slurm/
 *     chmod 755 /usr/lib/slurm/netns_isolate.so
 *
 *   Register in /etc/slurm/plugstack.conf:
 *     optional /usr/lib/slurm/netns_isolate.so partition=<p> netns=<n>
 *
 *   Reload:
 *     systemctl restart slurmd
 */

#define _GNU_SOURCE

#include <slurm/spank.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>     /* PATH_MAX */
#include <stdint.h>     /* uint32_t */
#include <stdio.h>      /* snprintf() */
#include <string.h>     /* strncmp(), strncpy() */
#include <unistd.h>     /* access() */

SPANK_PLUGIN(netns_isolate, 1);

#define IP_PATH    "/sbin/ip"
#define NETNS_DIR  "/run/netns"
#define NSNAME_MAX 64
#define PARTNAME_MAX 64

/* Configured via plugstack.conf arguments */
static char cfg_partition[PARTNAME_MAX] = "";
static char cfg_nsname[NSNAME_MAX]      = "";


/* ---------------------------------------------------------------------------
 * parse_opts()
 *
 * Reads partition= and netns= from plugstack.conf arguments.
 * Called from slurm_spank_init.
 * ------------------------------------------------------------------------- */
static int parse_opts(int ac, char **av)
{
    int i;

    for (i = 0; i < ac; i++) {
        if (strncmp(av[i], "partition=", 10) == 0) {
            strncpy(cfg_partition, av[i] + 10, sizeof(cfg_partition) - 1);
            cfg_partition[sizeof(cfg_partition) - 1] = '\0';
        } else if (strncmp(av[i], "netns=", 6) == 0) {
            strncpy(cfg_nsname, av[i] + 6, sizeof(cfg_nsname) - 1);
            cfg_nsname[sizeof(cfg_nsname) - 1] = '\0';
        } else {
            slurm_error("netns_isolate: unknown option '%s'", av[i]);
            return -1;
        }
    }

    if (!cfg_partition[0]) {
        slurm_error("netns_isolate: partition= is required");
        return -1;
    }
    if (!cfg_nsname[0]) {
        slurm_error("netns_isolate: netns= is required");
        return -1;
    }

    return 0;
}


int slurm_spank_init(spank_t sp, int ac, char **av)
{
    (void)sp;
    return parse_opts(ac, av);
}


/* ===========================================================================
 * TASK INIT - runs in the task child before exec()
 *
 * If the job's partition matches cfg_partition, checks that the namespace
 * file exists and prepends `ip netns exec <n>` to the task argv.
 * If the namespace file is missing, warns but does not fail — the job
 * runs in the default namespace rather than being blocked.
 * ========================================================================= */
int slurm_spank_task_init(spank_t sp, int ac, char **av)
{
    char job_partition[PARTNAME_MAX] = "";
    char nspath[PATH_MAX];
    const char *prepend[] = { IP_PATH, "netns", "exec", cfg_nsname };
    int n;

    (void)ac; (void)av;

    if (spank_context() != S_CTX_REMOTE)
        return 0;

    /* Check which partition this job was submitted to */
    if (spank_getenv(sp, "SLURM_JOB_PARTITION", job_partition,
                     sizeof(job_partition)) != ESPANK_SUCCESS) {
        slurm_error("netns_isolate: failed to get SLURM_JOB_PARTITION");
        return -1;
    }

    if (strncmp(job_partition, cfg_partition, PARTNAME_MAX) != 0)
        return 0;

    /* Build the expected namespace path */
    n = snprintf(nspath, sizeof(nspath), "%s/%s", NETNS_DIR, cfg_nsname);
    if (n <= 0 || (size_t)n >= sizeof(nspath)) {
        slurm_error("netns_isolate: namespace path truncated for '%s'",
                    cfg_nsname);
        return -1;
    }

    /* Check the namespace file exists before prepending */
    if (access(nspath, F_OK) < 0) {
        slurm_error("netns_isolate: namespace '%s' not found at %s — "
                      "job will run in default namespace. "
                      "Has the namespace been created on this node?",
                      cfg_nsname, nspath);
        return 0;
    }

    if (spank_prepend_task_argv(sp, 4, prepend) != ESPANK_SUCCESS) {
        slurm_error("netns_isolate: failed to prepend task argv for "
                    "namespace '%s'", cfg_nsname);
        return -1;
    }

    slurm_verbose("netns_isolate: task will run in namespace '%s'",
                  cfg_nsname);
    return 0;
}
