/*
 * SPANK plugin for partition/node-based network namespace isolation
 *
 * PURPOSE:
 *   Automatically moves job tasks into a pre-created named network namespace
 *   when the job is submitted to a designated partition AND runs on a node in
 *   a configured nodelist. Uses setns() in task_init_privileged (runs as root,
 *   pre-execve) so the job process inherits the namespace across the privilege
 *   drop and subsequent execve().
 *
 * CONFIGURATION:
 *   Arguments are set in plugstack.conf, e.g.:
 *     optional <SLURM_LIB_DIR>/netns_spank.so partition=isolated-jobs nodes=gpu[1-10],cpu[1-20] netns=isolated
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
 *   The namespace persists until the node reboots and is shared across jobs -
 *   it carries no per-job state. Multiple concurrent jobs safely share it.
 *
 * DEPLOYMENT:
 *   Build:
 *     gcc -std=c99 -shared -fPIC -Wall -Wextra -I<SLURM_INCLUDE_DIR>/slurm \
 *         -o netns_spank.so netns_spank.c -lslurm
 *
 *   Install:
 *     cp netns_spank.so <SLURM_LIB_DIR>
 *     chmod 755 <SLURM_LIB_DIR>/netns_spank.so
 *
 *   Register in /etc/slurm/plugstack.conf:
 *     optional <SLURM_LIB_DIR>/netns_spank.so partition=<p> nodes=<n> netns=<ns>
 *
 */

#define _GNU_SOURCE

#include <slurm/spank.h>
#include <slurm/slurm.h>    /* slurm_hostlist_create(), slurm_hostlist_find() */

#include <fcntl.h>
#include <limits.h>         /* PATH_MAX */
#include <sched.h>          /* setns(), CLONE_NEWNET */
#include <stdint.h>         /* uint32_t */
#include <string.h>         /* strncmp(), strncpy() */
#include <unistd.h>         /* access(), gethostname() */

SPANK_PLUGIN(netns_spank, 1);

#define NETNS_DIR    "/var/run/netns"
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
            slurm_error("netns_spank: unknown option '%s'", av[i]);
            return -1;
        }
    }

    if (!cfg_partition[0]) {
        slurm_error("netns_spank: partition= is required");
        return -1;
    }
    if (!cfg_nodelist[0]) {
        slurm_error("netns_spank: nodes= is required");
        return -1;
    }
    if (!cfg_nsname[0]) {
        slurm_error("netns_spank: netns= is required");
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
        slurm_error("netns_spank: failed to parse nodelist '%s'", nodelist);
        return -1;
    }

    found = slurm_hostlist_find(hl, nodename) >= 0;
    slurm_hostlist_destroy(hl);
    return found;
}


int slurm_spank_init(spank_t sp, int ac, char **av)
{
    /* Suppress unused parameter warnings */
    (void)sp;
    return parse_opts(ac, av);
}


/* ===========================================================================
 * TASK INIT PRIVILEGED - runs as root in the forked task child, before
 * become_user() and execve().
 *
 * Checks partition and node criteria. If both match and the namespace file
 * exists, enters the pre-created namespace via setns().
 * The namespace is inherited across the subsequent become_user() privilege
 * drop and execve(), so the job runs isolated.
 *
 * If the namespace file is missing, logs an error and allows the job to
 * continue in the default namespace rather than blocking it.
 * ========================================================================= */
int slurm_spank_task_init_privileged(spank_t sp, int ac, char **av)
{
    char job_partition[PARTNAME_MAX] = "";
    char nodename[256]               = "";
    char nspath[PATH_MAX];
    int n, rc, fd;

    /* Suppress unused parameter warnings */
    (void)ac; (void)av;

    if (spank_context() != S_CTX_REMOTE) {
        slurm_verbose("netns_spank: skipping - not running in remote task context");
        return 0;
    }

    /* Check partition */
    if (spank_getenv(sp, "SLURM_JOB_PARTITION", job_partition,
                     sizeof(job_partition)) != ESPANK_SUCCESS) {
        slurm_error("netns_spank: failed to get SLURM_JOB_PARTITION");
        return -1;
    }
    if (strncmp(job_partition, cfg_partition, PARTNAME_MAX) != 0)
        return 0;

    /* Check node name. Can't use HOSTNAME env var because for direct srun
     * calls HOSTNAME remains set to the submit node; use gethostname().
     */
    if (gethostname(nodename, sizeof(nodename)) < 0) {
        slurm_error("netns_isolate: gethostname() failed: %m");
        return -1;
    }

    rc = node_in_list(nodename, cfg_nodelist);
    if (rc < 0) {
        slurm_verbose("netns_spank: failed to parse nodelist '%s'", cfg_nodelist);
        return -1;
    }
    if (rc == 0) {
        slurm_verbose("netns_spank: node '%s' not in configured nodelist '%s' - "
                    "job will run in default namespace",
                    nodename, cfg_nodelist);
        return 0;
    }

    /* Build the expected namespace path */
    n = snprintf(nspath, sizeof(nspath), "%s/%s", NETNS_DIR, cfg_nsname);
    if (n <= 0 || (size_t)n >= sizeof(nspath)) {
        slurm_error("netns_spank: namespace path truncated for '%s'",
                    cfg_nsname);
        return -1;
    }

    /* Warn but don't fail if namespace doesn't exist on this node */
    if (access(nspath, F_OK) < 0) {
        slurm_error("netns_spank: namespace '%s' not found at %s - "
                    "job will run in default namespace. "
                    "Has the namespace been created on this node?",
                    cfg_nsname, nspath);
        return 0;
    }

    fd = open(nspath, O_RDONLY);
    if (fd < 0) {
        slurm_error("netns_spank: open(%s): %m", nspath);
        return -1;
    }

    /*
     * Enter the namespace. This affects only the current forked task child -
     * slurmstepd is unaffected. The namespace is inherited across the
     * subsequent become_user() and execve(), so the job runs isolated.
     */
    if (setns(fd, CLONE_NEWNET) < 0) {
        slurm_error("netns_spank: setns(%s): %m", nspath);
        close(fd);
        return -1;
    }

    close(fd);
    slurm_verbose("netns_spank: task on %s entered namespace '%s'",
                  nodename, cfg_nsname);
    return 0;
}
