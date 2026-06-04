/*
 * SPANK plugin for running jobs in a network namespace.
 *
 * PURPOSE:
 *   Automatically moves job tasks into a pre-created named network namespace
 *   when the job is submitted to a designated partition. Uses setns() in
 *   task_init_privileged (runs as root, pre-execve) so the job process
 *   inherits the namespace across the privilege drop and subsequent
 *   execve(). The network namespace is entered first, then it mimics the
 *   behavior of 'ip netns exec' by also creating a temporary mount namspace
 *   in which it mounts items found under /etc/netns/<nsname>/.
 *
 * CONFIGURATION:
 *   Arguments are set in plugstack.conf, e.g.:
 *     optional <SLURM_LIB_DIR>/netns_spank.so partition=isolated-jobs netns=/var/run/netns/isolated
 *
 *   partition= : jobs must be submitted to this slurm partition (required)
 *   netns=     : full path to the pre-created network namespace (required)
 *
 * PRE-REQUISITES:
 *   The namespaces must be created on each applicable compute node, e.g.:
 *     ip netns add isolated
 *     ip netns exec isolated ip link set lo up
 *
 *   This creates the network namespace at /var/run/netns/<name>. The namespace
 *   persists until the node reboots and is shared across jobs - it carries
 *   no per-job state. Multiple concurrent jobs safely share it.
 *
 * DEPLOYMENT:
 *   Build:
 *     gcc -std=c99 -shared -fPIC -Wall -Wextra -I<SLURM_INCLUDE_DIR> \
 *         -o netns_spank.so netns_spank.c
 *
 *   Install:
 *     cp netns_spank.so <SLURM_LIB_DIR>
 *     chmod 755 <SLURM_LIB_DIR>/netns_spank.so
 *
 *   Register in /etc/slurm/plugstack.conf:
 *     optional <SLURM_LIB_DIR>/netns_spank.so partition=<p> netns=<ns>
 *
 */

#include "netns_common.h"

SPANK_PLUGIN(netns_spank, 1);

/* Configured via plugstack.conf arguments */
static char cfg_partition[PARTNAME_MAX] = "";
static char cfg_netns[PATH_MAX]         = "";

/* Guard */
int entered_netns = 0;

/* ---------------------------------------------------------------------------
 * parse_opts()
 *
 * Reads partition= and netns= from plugstack.conf arguments.
 * Returns 0 on success and -1 on failure.
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
            log_error("Unknown option '%s'", av[i]);
            return RC_UNKNOWN_OPT;
        }
    }

    if (!cfg_partition[0]) {
        log_error("partition= is required");
        return RC_MISSING_CONFIG;
    }
    if (!cfg_netns[0]) {
        log_error("netns= is required");
        return RC_MISSING_CONFIG;
    }

    return 0;
}

int slurm_spank_init(spank_t sp, int ac, char **av)
{
    /* Suppress unused parameter warnings */
    (void)sp;
    return parse_opts(ac, av);
}

int plugin(spank_t sp)
{
    char job_partition[PARTNAME_MAX] = "";
    int rc;

    /* Check context */
    if (spank_context() != S_CTX_REMOTE) {
        log_verbose("Skipping plugin - not running in remote task context");
        return RC_NOT_REMOTE_CTX;
    }

    /* Get partition */
    if (spank_getenv(sp, "SLURM_JOB_PARTITION", job_partition, sizeof(job_partition)) != ESPANK_SUCCESS) {
        log_error("Failed to get SLURM_JOB_PARTITION");
        return RC_GETENV_FAIL;
    }
    /* Check partition */
    if (strncmp(job_partition, cfg_partition, PARTNAME_MAX) != 0)
        return RC_WRONG_PARTITION;

    // Get the namespace name from the path, e.g. "external" from "/var/run/netns/external"
    const char *ns_name = strrchr(cfg_netns, '/');
    if (ns_name) {
        ns_name++;  // Move past the '/'
    } else {
        ns_name = cfg_netns;  // If no '/' found, use the whole string
    }

    if ( entered_netns == 1 ) {
        log_verbose("Already entered network namespace, skipping");
        return 0;
    }

    rc = netns_switch(ns_name);
    if (rc == 0)
        entered_netns = 1;
    return rc;


}

/* ===========================================================================
 * Configuration and setup errors are logged but gracefully skipped so the job
 * runs in the default namespace. Only slurm_spank_init() errors are fatal.
 * ========================================================================= */
int slurm_spank_task_init_privileged(spank_t sp, int ac, char **av)
{
    /* Suppress unused parameter warnings */
    (void)ac; (void)av;

    /* Enter network namespace and set up bind mounts */
    int rc = plugin(sp);
    if (rc != 0) {
        log_verbose("Error during network namespace plugin (%d)", rc);
#ifdef TEST
        return rc;
#else
        return 0;  /* Exit gracefully in production */
#endif
    }
    return 0;
}
