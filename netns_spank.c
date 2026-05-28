/*
 * SPANK plugin for running jobs in a network namespace and a mount namespace.
 *
 * PURPOSE:
 *   Automatically moves job tasks into a pre-created named network namespace
 *   and a mount namespace when the job is submitted to a designated
 *   partition. Uses setns() in task_init_privileged (runs as root, pre-execve)
 *   so the job process inherits the namespaces across the privilege drop and
 *   subsequent execve(). The network namespace is entered first, followed by
 *   the mount namespace.
 *
 * CONFIGURATION:
 *   Arguments are set in plugstack.conf, e.g.:
 *     optional <SLURM_LIB_DIR>/netns_spank.so partition=isolated-jobs netns=/var/run/netns/isolated mntns=/var/run/mntns/isolated
 *
 *   partition= : jobs must be submitted to this slurm partition (required)
 *   netns=     : full path to the pre-created network namespace (required)
 *   mntns=     : full path to the pre-created mount namespace (required)
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
 *   For mount namespaces, you must create and persist the namespace separately,
 *   e.g., by bind-mounting from a running process's mount namespace:
 *     mount --bind /proc/<pid>/ns/mnt /var/run/mntns/isolated_mnt
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
 *     optional <SLURM_LIB_DIR>/netns_spank.so partition=<p> netns=<ns> mntns=<mnt>
 *
 */

#define _GNU_SOURCE
#define PARTNAME_MAX 64
#define NS_NAME_MAX 16

/* Return codes for error paths*/

#define RC_MISSING_CONFIG 1
#define RC_UNKNOWN_OPT 2
#define RC_NOT_REMOTE_CTX 3
#define RC_GETENV_FAIL 4
#define RC_WRONG_PARTITION 5
#define RC_NO_NAMESPACE 6
#define RC_NAMESPACE_OPEN_FAIL 7
#define RC_NAMESPACE_NOT_ROOT 8
#define RC_SETNS_FAIL 9
#define RC_UNKNOWN_NS_TYPE 10

#include <slurm/spank.h>

#ifdef DEBUG
// Redefine logging macros to print to stdout/stderr for testing
// Undefine the extern function declarations from spank.h first
#undef slurm_verbose
#undef slurm_error
#define slurm_verbose(...) fprintf(stdout, __VA_ARGS__), fprintf(stdout, "\n")
#define slurm_error(...) fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\n")
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>         /* PATH_MAX */
#include <sched.h>          /* setns(), CLONE_NEWNS, CLONE_NEWNET, etc... */
#include <string.h>         /* strncmp(), strncpy() */
#include <sys/stat.h>       /* fstat() */
#include <unistd.h>         /* access() */
#include <stdio.h>          /* snprintf() */

SPANK_PLUGIN(netns_spank, 1);

/* Configured via plugstack.conf arguments */
static char cfg_partition[PARTNAME_MAX] = "";
static char cfg_netns[PATH_MAX]         = "";
static char cfg_mntns[PATH_MAX]         = "";

int entered_netns = 0;
int entered_mntns = 0;


/* ---------------------------------------------------------------------------
 * parse_opts()
 *
 * Reads partition=, netns=, and mntns= from plugstack.conf arguments.
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
        } else if (strncmp(av[i], "mntns=", 6) == 0) {
            strncpy(cfg_mntns, av[i] + 6, sizeof(cfg_mntns) - 1);
            cfg_mntns[sizeof(cfg_mntns) - 1] = '\0';
        } else {
            slurm_error("netns_spank: unknown option '%s'", av[i]);
            return RC_UNKNOWN_OPT;
        }
    }

    if (!cfg_partition[0]) {
        slurm_error("netns_spank: partition= is required");
        return RC_MISSING_CONFIG;
    }
    if (!cfg_netns[0]) {
        slurm_error("netns_spank: netns= is required");
        return RC_MISSING_CONFIG;
    }
    if (!cfg_mntns[0]) {
        slurm_error("netns_spank: mntns= is required");
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
        default:
            slurm_error("netns_spank: invalid namespace type %d", ns_type);
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
            slurm_error("netns_spank: %s namespace '%s' not found - "
                        "job will run in default %s namespace. "
                        "Has the namespace been created on this node?",
                        ns_name, ns_path, ns_name);
            return RC_NO_NAMESPACE;
        }
        slurm_error("netns_spank: open(%s): %m", ns_path);
        return RC_NAMESPACE_OPEN_FAIL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_uid != 0) {
        slurm_error("netns_spank: %s namespace not owned by root!", ns_name);
        close(fd);
        return RC_NAMESPACE_NOT_ROOT;
    }

    /*
     * Enter the namespace. This affects only the current forked task child.
     * The namespace is inherited across the subsequent become_user() and execve().
     */
    if (setns(fd, ns_type) < 0) {
        slurm_error("netns_spank: setns(%s, '%s'): %m", ns_path, ns_name);
        close(fd);
        return RC_SETNS_FAIL;
    }

    close(fd);
    slurm_verbose("netns_spank: task entered %s namespace '%s'", ns_name, ns_path);
    return 0;
}


/* ===========================================================================
 * If the job's partition matches cfg_partition, enters the pre-created
 * namespace via setns(). The namespace is inherited across the subsequent
 * become_user() privilege drop and execve(), so the job runs in that namespace.
 * ========================================================================= */
int namespace_plugin(spank_t sp, int ac, char **av)
{
    char job_partition[PARTNAME_MAX] = "";
    int rc;

    /* Suppress unused parameter warnings */
    (void)ac; (void)av;

    if (spank_context() != S_CTX_REMOTE) {
        slurm_verbose("netns_spank: skipping - not running in remote task context");
        return RC_NOT_REMOTE_CTX;
    }

    /* Check partition */
    if (spank_getenv(sp, "SLURM_JOB_PARTITION", job_partition,
                     sizeof(job_partition)) != ESPANK_SUCCESS) {
        slurm_error("netns_spank: failed to get SLURM_JOB_PARTITION");
        return RC_GETENV_FAIL;
    }
    if (strncmp(job_partition, cfg_partition, PARTNAME_MAX) != 0)
        return RC_WRONG_PARTITION;  /* not our partition */

    /* Enter network namespace */
    rc = enter_namespace(cfg_netns, CLONE_NEWNET);
    if (rc > 0) {
        slurm_verbose("netns_spank: failed to enter network namespace -- skipping mount namespace setup");
        return rc;
    }
    else {
        entered_netns = 1;
    }

    /* Enter mount namespace */
    rc = enter_namespace(cfg_mntns, CLONE_NEWNS);
    if (rc > 0) {
        return rc;
    }
    else {
        entered_mntns = 1;
    }

    slurm_verbose("netns_spank: namespace setup complete for job in partition '%s'", job_partition);
    return 0;
}

/* ===========================================================================
 * Configuration and setup errors are logged but gracefully skipped so the job
 * runs in the default namespace. Only true plugin errors are fatal.
 * ========================================================================= */
int slurm_spank_init_post_opt(spank_t sp, int ac, char **av)
{
    int rc = namespace_plugin(sp, ac, av);
    if (rc != 0) {
        slurm_verbose("netns_spank: error '%d' during namespace setup", rc);
#ifdef DEBUG
        return rc;
#else
        return 0;  /* Exit gracefully in production */
#endif
    } else {
        return 0;
    }
}
