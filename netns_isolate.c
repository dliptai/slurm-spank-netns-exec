/*
 * SPANK plugin for partition/node-based network namespace isolation
 *
 * PURPOSE:
 *   Automatically runs job tasks inside a pre-created network namespace when
 *   the job is submitted to a designated partition AND runs on a node in a
 *   configured nodelist. Uses `ip netns exec` via spank_prepend_task_argv().
 *
 * CONFIGURATION:
 *   Arguments are set in plugstack.conf, e.g.:
 *     optional /usr/lib/slurm/netns_isolate.so partition=isolated-jobs nodes=gpu[1-10],cpu[1-20] netns=isolated
 *
 *   partition= : jobs must be submitted to this partition (required)
 *   nodes=     : jobs must be running on a node in this list (required)
 *                accepts standard Slurm hostlist notation: node[1-10],node20
 *   netns=     : name of the pre-created network namespace (required)
 *
 * PRE-REQUISITES:
 *   The namespace must be created on each applicable compute node, e.g.:
 *     ip netns add isolated
 *     ip netns exec isolated ip link set lo up
 *
 *   Per-namespace config (e.g. custom resolv.conf) can be placed in
 *   /etc/netns/<n>/ and will be bind-mounted automatically by ip.
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
 *     optional /usr/lib/slurm/netns_isolate.so partition=<p> nodes=<n> netns=<n>
 *
 */

#define _GNU_SOURCE

#include <slurm/spank.h>
#include <slurm/slurm.h>    /* slurm_hostlist_create(), slurm_hostlist_find() */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>         /* PATH_MAX */
#include <stdint.h>         /* uint32_t */
#include <string.h>         /* strncmp(), strncpy() */
#include <unistd.h>         /* access() */

SPANK_PLUGIN(netns_isolate, 1);

#define IP_PATH      "/sbin/ip"
#define NETNS_DIR    "/run/netns"
#define NSNAME_MAX   64
#define PARTNAME_MAX 64
#define NODELIST_MAX 1024

/* Configured via plugstack.conf arguments */
static char cfg_partition[PARTNAME_MAX] = "";
static char cfg_nodelist[NODELIST_MAX]  = "";
static char cfg_nsname[NSNAME_MAX]      = "";


/* ---------------------------------------------------------------------------
 * parse_opts()
 *
 * Reads partition=, nodes=, and netns= from plugstack.conf arguments.
 * ------------------------------------------------------------------------- */
static int parse_opts(int ac, char **av)
{
    int i;

    for (i = 0; i < ac; i++) {
        if (strncmp(av[i], "partition=", 10) == 0) {
            strncpy(cfg_partition, av[i] + 10, sizeof(cfg_partition) - 1);
            cfg_partition[sizeof(cfg_partition) - 1] = '\0';
        } else if (strncmp(av[i], "nodes=", 6) == 0) {
            strncpy(cfg_nodelist, av[i] + 6, sizeof(cfg_nodelist) - 1);
            cfg_nodelist[sizeof(cfg_nodelist) - 1] = '\0';
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
    if (!cfg_nodelist[0]) {
        slurm_error("netns_isolate: nodes= is required");
        return -1;
    }
    if (!cfg_nsname[0]) {
        slurm_error("netns_isolate: netns= is required");
        return -1;
    }

    return 0;
}


/* ---------------------------------------------------------------------------
 * node_in_list()
 *
 * Returns 1 if nodename appears in the Slurm hostlist expression nodelist,
 * 0 if not, -1 on error. Uses the Slurm hostlist API to correctly handle
 * bracket notation such as node[1-10],gpu[01-04].
 * ------------------------------------------------------------------------- */
static int node_in_list(const char *nodename, const char *nodelist)
{
    hostlist_t *hl;
    int found;

    hl = slurm_hostlist_create(nodelist);
    if (!hl) {
        slurm_error("netns_isolate: failed to parse nodelist '%s'", nodelist);
        return -1;
    }

    found = slurm_hostlist_find(hl, nodename) >= 0;
    slurm_hostlist_destroy(hl);
    return found;
}


int slurm_spank_init(spank_t sp, int ac, char **av)
{
    (void)sp;
    return parse_opts(ac, av);
}


/* ===========================================================================
 * TASK INIT - runs in the task child before exec()
 *
 * Checks partition and node criteria. If both match and the namespace file
 * exists, prepends `ip netns exec <n>` to the task argv. If the namespace
 * file is missing, logs an error and warns but does not fail the job.
 * ========================================================================= */
int slurm_spank_task_init(spank_t sp, int ac, char **av)
{
    char job_partition[PARTNAME_MAX] = "";
    char nodename[256]               = "";
    char nspath[PATH_MAX];
    const char *prepend[] = { IP_PATH, "netns", "exec", cfg_nsname };
    int n, rc;

    (void)ac; (void)av;

    if (spank_context() != S_CTX_REMOTE)
        return 0;

    /* Check partition */
    if (spank_getenv(sp, "SLURM_JOB_PARTITION", job_partition,
                     sizeof(job_partition)) != ESPANK_SUCCESS) {
        slurm_error("netns_isolate: failed to get SLURM_JOB_PARTITION");
        return -1;
    }
    if (strncmp(job_partition, cfg_partition, PARTNAME_MAX) != 0)
        return 0;

    /* Check node — SLURMD_NODENAME is set by slurmd on each compute node */
    if (spank_getenv(sp, "SLURMD_NODENAME", nodename,
                     sizeof(nodename)) != ESPANK_SUCCESS) {
        slurm_error("netns_isolate: failed to get SLURMD_NODENAME");
        return -1;
    }

    rc = node_in_list(nodename, cfg_nodelist);
    if (rc < 0)
        return -1;
    if (rc == 0)
        return 0;

    /* Build the expected namespace path */
    n = snprintf(nspath, sizeof(nspath), "%s/%s", NETNS_DIR, cfg_nsname);
    if (n <= 0 || (size_t)n >= sizeof(nspath)) {
        slurm_error("netns_isolate: namespace path truncated for '%s'",
                    cfg_nsname);
        return -1;
    }

    /* Warn but don't fail if namespace doesn't exist on this node */
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

    slurm_verbose("netns_isolate: task on %s will run in namespace '%s'",
                  nodename, cfg_nsname);
    return 0;
}
