/*
 * SPANK plugin for running jobs in a network namespace.
 *
 * PURPOSE:
 *   Automatically moves job tasks into a pre-created named network namespace
 *   when the job is submitted to a designated partition. Uses setns() in
 *   task_init_privileged (runs as root, pre-execve) so the job process
 *   inherits the namespace across the privilege drop and subsequent execve().
 *
 * CONFIGURATION:
 *   Arguments are set in plugstack.conf, e.g.:
 *     optional <SLURM_LIB_DIR>/netns_spank.so partition=isolated-jobs netns=/var/run/netns/isolated
 *
 *   partition= : jobs must be submitted to this slurm partition (required)
 *   netns=     : full path to the pre-created network namespace (required)
 *
 * PRE-REQUISITES:
 *   The namespace must be created on each applicable compute node, e.g.:
 *     ip netns add isolated
 *     ip netns exec isolated ip link set lo up
 *
 *   This creates the namespace at /var/run/netns/<name>. The namespace
 *   persists until the node reboots and is shared across jobs - it carries
 *   no per-job state. Multiple concurrent jobs safely share it.
 *
 * DEPLOYMENT:
 *   Build:
 *     gcc -std=c99 -shared -fPIC -Wall -Wextra -I<SLURM_INCLUDE_DIR> \
 *         -o netns_spank.so netns_spank.c -lslurm
 *
 *   Install:
 *     cp netns_spank.so <SLURM_LIB_DIR>
 *     chmod 755 <SLURM_LIB_DIR>/netns_spank.so
 *
 *   Register in /etc/slurm/plugstack.conf:
 *     optional <SLURM_LIB_DIR>/netns_spank.so partition=<p> netns=<ns>
 *
 */

#define _GNU_SOURCE

#include <slurm/spank.h>

#include <fcntl.h>
#include <limits.h>         /* PATH_MAX */
#include <sched.h>          /* setns(), CLONE_NEWNET */
#include <string.h>         /* strncmp(), strncpy() */
#include <unistd.h>         /* access() */
#include <stdio.h>          /* snprintf() */

SPANK_PLUGIN(netns_spank, 1);

#define PARTNAME_MAX 64

/* Configured via plugstack.conf arguments */
static char cfg_partition[PARTNAME_MAX] = "";
static char cfg_netns[PATH_MAX]         = "";


/* ---------------------------------------------------------------------------
 * parse_opts()
 *
 * Reads partition= and netns= from plugstack.conf arguments.
 * ------------------------------------------------------------------------- */
static int parse_opts(int ac, char **av)
{
    for (int i = 0; i < ac; i++) {
        if (strncmp(av[i], "partition=", 10) == 0) {
            strncpy(cfg_partition, av[i] + 10, sizeof(cfg_partition) - 1);
            cfg_partition[sizeof(cfg_partition) - 1] = '\0';
        } else if (strncmp(av[i], "netns=", 6) == 0) {
            strncpy(cfg_netns, av[i] + 6, sizeof(cfg_netns) - 1);
            cfg_netns[sizeof(cfg_netns) - 1] = '\0';
        } else {
            slurm_error("netns_spank: unknown option '%s'", av[i]);
            return -1;
        }
    }

    if (!cfg_partition[0]) {
        slurm_error("netns_spank: partition= is required");
        return -1;
    }
    if (!cfg_netns[0]) {
        slurm_error("netns_spank: netns= is required");
        return -1;
    }

    return 0;
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
 * If the job's partition matches cfg_partition, enters the pre-created
 * namespace via setns(). The namespace is inherited across the subsequent
 * become_user() privilege drop and execve(), so the job runs in that namespace.
 *
 * If the namespace file is missing, logs an error and allows the job to
 * continue in the default namespace rather than blocking it.
 * ========================================================================= */
int slurm_spank_task_init_privileged(spank_t sp, int ac, char **av)
{
    char job_partition[PARTNAME_MAX] = "";
    int fd;

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

    /* Warn but don't fail if namespace doesn't exist on this node */
    if (access(cfg_netns, F_OK) < 0) {
        slurm_error("netns_spank: namespace '%s' not found at %s - "
                    "job will run in default namespace. "
                    "Has the namespace been created on this node?",
                    cfg_netns, cfg_netns);
        return 0;
    }

    fd = open(cfg_netns, O_RDONLY);
    if (fd < 0) {
        slurm_error("netns_spank: open(%s): %m", cfg_netns);
        return -1;
    }

    /*
     * Enter the namespace. This affects only the current forked task child -
     * slurmstepd is unaffected. The namespace is inherited across the
     * subsequent become_user() and execve(), so the job runs in that namespace.
     */
    if (setns(fd, CLONE_NEWNET) < 0) {
        slurm_error("netns_spank: setns(%s): %m", cfg_netns);
        close(fd);
        return -1;
    }

    close(fd);
    slurm_verbose("netns_spank: task entered namespace '%s'", cfg_netns);
    return 0;
}
