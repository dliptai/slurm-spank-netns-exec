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

int entered_netns = 0;
int entered_mntns = 0;


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


/* ===========================================================================
 * enter_namespace()
 *
 * Helper function to enter a namespace file. Opens the namespace file,
 * validates it is owned by root, and calls setns() with the specified
 * namespace type.
 *
 * Parameters:
 *   ns_path   - Path to the namespace file
 *   ns_type   - Namespace type (e.g., CLONE_NEWNET, CLONE_NEWNS)
 *
 * ========================================================================= */
static int enter_namespace(const char *ns_path, int ns_type)
{
    int fd;

    // For logging purposes, convert ns_type to a string
    char ns_name[NS_NAME_MAX];
    switch (ns_type) {
        case CLONE_NEWCGROUP: strncpy(ns_name, "cgroup", sizeof(ns_name) - 1); break;
        case CLONE_NEWIPC:    strncpy(ns_name, "ipc", sizeof(ns_name) - 1); break;
        case CLONE_NEWNET:    strncpy(ns_name, "network", sizeof(ns_name) - 1); break;
        case CLONE_NEWNS:     strncpy(ns_name, "mount", sizeof(ns_name) - 1); break;
        case CLONE_NEWPID:    strncpy(ns_name, "pid", sizeof(ns_name) - 1); break;
        case CLONE_NEWUSER:   strncpy(ns_name, "user", sizeof(ns_name) - 1); break;
        case CLONE_NEWUTS:    strncpy(ns_name, "uts", sizeof(ns_name) - 1); break;
        case 0:               strncpy(ns_name, "unspecified", sizeof(ns_name) - 1); break;
        default:
            log_error("Invalid namespace type %d", ns_type);
            return RC_UNKNOWN_NS_TYPE;
    }

    /*
     * Open the namespace file with secure flags:
     *   O_RDONLY       - read-only (we only need to pass it to setns)
     *   O_CLOEXEC      - close on execve() to prevent leaking the fd to the job
     *   O_NOFOLLOW     - fail if ns_path is a symlink (prevents symlink attacks)
     */
    fd = open(ns_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) {
            log_error("%s namespace '%s' not found - job will run in default %s namespace. Has the namespace been created on this node?",
                        ns_name, ns_path, ns_name);
            return RC_NO_NAMESPACE;
        }
        log_error("open(%s): %m", ns_path);
        return RC_NAMESPACE_OPEN_FAIL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_uid != 0) {
        log_error("%s namespace not owned by root!", ns_name);
        close(fd);
        return RC_NAMESPACE_NOT_ROOT;
    }

    /*
     * Enter the namespace. This affects only the current forked task child.
     * The namespace is inherited across the subsequent become_user() and execve().
     */
    if (setns(fd, ns_type) < 0) {
        log_error("setns() fails with path '%s', type '%s'): %m", ns_path, ns_name);
        close(fd);
        return RC_SETNS_FAIL;
    }

    close(fd);
    log_verbose("Task entered %s namespace '%s'", ns_name, ns_path);
    return 0;
}

int enter_network_namespace(spank_t sp)
{
    int rc;

    (void)sp;  /* unused */

    if ( entered_netns == 1 ) {
        log_verbose("Already entered network namespace, skipping");
        return 0;
    }

    /* Enter network namespace */
    rc = enter_namespace(cfg_netns, CLONE_NEWNET);
    if (rc > 0) {
        log_verbose("Failed to enter network namespace");
        return rc;
    }
    entered_netns = 1;

    return 0;
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


    /* Enter network namespace if not already entered */
    if ( entered_netns == 0 ) {
        rc = enter_namespace(cfg_netns, CLONE_NEWNET);
        if (rc > 0) {
            log_verbose("Failed to enter network namespace");
            return rc; // Do not try to create bind mounts if we failed to enter the network namespace
        }
    } else {
        log_verbose("Already entered network namespace, skipping");
    }

    if ( entered_mntns == 1) {
        log_verbose("Already entered mount namespace, skipping");
        return 0;
    }

    /* Create mount namespace and bind mounts if not already done so */

    // Get the namespace name from the path, e.g. "external" from "/var/run/netns/external"
    const char *ns_name = strrchr(cfg_netns, '/');
    if (ns_name) {
        ns_name++;  // Move past the '/'
    } else {
        ns_name = cfg_netns;  // If no '/' found, use the whole string
    }

    return iproute_bind_mounts(ns_name);

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
