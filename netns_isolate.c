/*
 * netns_isolate.c - SPANK plugin for per-job network namespace isolation
 *
 * PURPOSE:
 *   Creates a private, isolated network namespace for each Slurm job.
 *   The namespace has only a loopback interface and no external routes,
 *   blocking all outbound network connections from job processes.
 *   Applies to all tasks regardless of submission method (sbatch or srun).
 *
 * NAMESPACE NAMING:
 *   Namespaces are named job-<jobid>. Slurm assigns a unique job ID to every
 *   job, array task, and heterogeneous job component, so this is always
 *   sufficient to guarantee uniqueness on a node.
 *
 * LIFECYCLE:
 *   slurm_spank_job_prolog           - creates the namespace
 *   slurm_spank_task_init_privileged - enters the namespace before task exec
 *   slurm_spank_job_epilog           - destroys the namespace
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
 *     required /usr/lib/slurm/netns_isolate.so
 *
 *   Use "required" so that if the plugin fails, the job fails rather than
 *   silently running with full network access. Use "optional" only for
 *   initial testing on a non-production partition.
 *
 *   Reload:
 *     systemctl restart slurmd
 */

/* Must be defined before any header is included - features.h (pulled in by
 * virtually every header) uses this to gate Linux-specific prototypes such as
 * unshare(), setns(), and CLONE_NEWNET from <sched.h>. */
#define _GNU_SOURCE

#include <slurm/spank.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>     /* PATH_MAX */
#include <sched.h>      /* unshare(), setns(), CLONE_NEWNET */
#include <stdint.h>     /* uint32_t */
#include <stdio.h>      /* snprintf(), perror() */
#include <string.h>     /* memset(), memcpy() */
#include <unistd.h>     /* getuid(), fork(), _exit() */

#include <net/if.h>     /* struct ifreq, IFNAMSIZ, IFF_UP, IFF_RUNNING */
#include <sys/ioctl.h>  /* ioctl(), SIOCGIFFLAGS, SIOCSIFFLAGS */
#include <sys/mount.h>  /* mount(), umount2(), MS_BIND, MNT_DETACH */
#include <sys/socket.h> /* socket(), AF_INET, SOCK_DGRAM */
#include <sys/stat.h>   /* mkdir() */
#include <sys/wait.h>   /* waitpid(), WIFEXITED(), WEXITSTATUS(),
                           WIFSIGNALED(), WTERMSIG() */

SPANK_PLUGIN(netns_isolate, 1);

#define NETNS_DIR    "/run/netns"
#define PROC_SELF_NS "/proc/self/ns/net"


/* ---------------------------------------------------------------------------
 * get_ns_path()
 *
 * Builds the namespace path /run/netns/job-<jobid> into buf (size len).
 * Called at the top of each hook. The hook name is passed in for use in
 * error messages.
 *
 * Returns 0 on success, -1 on failure.
 * ------------------------------------------------------------------------- */
static int get_ns_path(spank_t sp, char *buf, size_t len, const char *hook)
{
    uint32_t jobid;
    int n;

    if (spank_get_item(sp, S_JOB_ID, &jobid) != ESPANK_SUCCESS) {
        slurm_error("netns_isolate: %s: failed to get job ID", hook);
        return -1;
    }

    n = snprintf(buf, len, "%s/job-%u", NETNS_DIR, jobid);
    if (n <= 0 || (size_t)n >= len) {
        slurm_error("netns_isolate: %s: namespace path truncated", hook);
        return -1;
    }
    return 0;
}


/* ---------------------------------------------------------------------------
 * bring_up_loopback()
 *
 * Brings the loopback interface up inside the current network namespace.
 * Called from within the child process after unshare(CLONE_NEWNET).
 *
 * A new namespace always starts with lo DOWN - without this, even localhost
 * communication within the job would fail.
 *
 * Uses perror() rather than slurm_error() as logging may not be safe
 * post-fork in all Slurm versions.
 *
 * Returns 0 on success, -1 on failure.
 * ------------------------------------------------------------------------- */
static int bring_up_loopback(void)
{
    struct ifreq ifr;
    int sock;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("netns_isolate: socket");
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    memcpy(ifr.ifr_name, "lo", 2); /* memset above guarantees null termination */

    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
        perror("netns_isolate: SIOCGIFFLAGS");
        close(sock);
        return -1;
    }

    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;

    if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) {
        perror("netns_isolate: SIOCSIFFLAGS");
        close(sock);
        return -1;
    }

    close(sock);
    return 0;
}


/* ===========================================================================
 * INIT - runs in all contexts when the plugin is loaded
 *
 * Logs a confirmation that the plugin loaded successfully. Useful for
 * verifying the plugin is active without needing to run a job - check
 * with: journalctl -u slurmd | grep netns_isolate
 * ========================================================================= */
int slurm_spank_init(spank_t sp, int ac, char **av)
{
    (void)sp; (void)ac; (void)av;
    slurm_verbose("netns_isolate: plugin loaded (context=%s)",
                  spank_context() == S_CTX_REMOTE ? "remote" : "local");
    return 0;
}


/* ===========================================================================
 * PROLOG - runs as root on the compute node before any tasks are launched
 *
 * Creates an isolated network namespace for the job and persists it via
 * bind-mount so it survives until the epilog cleans it up.
 * ========================================================================= */
int slurm_spank_job_prolog(spank_t sp, int ac, char **av)
{
    char nspath[PATH_MAX];
    pid_t pid;
    int status, fd;
    uid_t uid;

    (void)ac; (void)av;

    if (spank_context() != S_CTX_REMOTE)
        return 0;

    uid = getuid();
    if (uid != 0) {
        slurm_error("netns_isolate: prolog requires root (uid=%u)", uid);
        return -1;
    }

    if (get_ns_path(sp, nspath, sizeof(nspath), "prolog") < 0)
        return -1;

    /* Create /run/netns/ if it doesn't exist */
    if (mkdir(NETNS_DIR, 0755) < 0 && errno != EEXIST) {
        slurm_error("netns_isolate: prolog: mkdir(%s): %m", NETNS_DIR);
        return -1;
    }

    /*
     * Create an empty bind-mount target file. O_EXCL catches leftovers from
     * crashed jobs. Two cases:
     *   - File is a live bind-mount from a crashed job that reused this ID:
     *     safe to reuse, task_init will enter it normally.
     *   - File is a plain empty file from a prolog that crashed after open()
     *     but before mount(): task_init will open it successfully but setns()
     *     will fail with EINVAL. Manual cleanup required: rm <nspath>.
     * Both are unlikely - we warn and continue rather than failing the job.
     */
    fd = open(nspath, O_RDONLY | O_CREAT | O_EXCL, 0);
    if (fd < 0) {
        if (errno == EEXIST) {
            slurm_info("netns_isolate: prolog: %s already exists "
                       "(leftover from crashed job?), reusing", nspath);
            return 0;
        }
        slurm_error("netns_isolate: prolog: open(%s): %m", nspath);
        return -1;
    }
    close(fd);

    /*
     * Fork before calling unshare(). We cannot unshare in slurmstepd itself
     * - it would lose its cluster network connection. The child unshares,
     * sets up the namespace, bind-mounts it for persistence, then exits.
     * The namespace survives the child exiting via the bind-mount.
     */
    pid = fork();
    if (pid < 0) {
        slurm_error("netns_isolate: prolog: fork(): %m");
        unlink(nspath);
        return -1;
    }

    if (pid == 0) {
        /* === CHILD - move into a new network namespace === */

        if (unshare(CLONE_NEWNET) < 0) {
            perror("netns_isolate: unshare");
            _exit(1);
        }

        if (bring_up_loopback() < 0)
            _exit(1);

        /*
         * Bind-mount /proc/self/ns/net (now pointing at our new namespace)
         * onto nspath to keep the namespace alive after this child exits.
         */
        if (mount(PROC_SELF_NS, nspath, "none", MS_BIND, NULL) < 0) {
            perror("netns_isolate: mount");
            _exit(1);
        }

        _exit(0);
    }

    /* Wait for child; retry on signal interruption */
    do {
        pid = waitpid(pid, &status, 0);
    } while (pid < 0 && errno == EINTR);

    if (pid < 0) {
        slurm_error("netns_isolate: prolog: waitpid(): %m");
        /*
         * Child outcome is unknown. unlink() will remove the target file but
         * if the child had already called mount() a stale bind-mount may
         * remain. Check /run/netns/ for leftover entries and unmount manually
         * if needed: umount /run/netns/job-<id> && rm /run/netns/job-<id>
         */
        unlink(nspath);
        return -1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFSIGNALED(status))
            slurm_error("netns_isolate: prolog: child killed by signal %d",
                        WTERMSIG(status));
        else
            slurm_error("netns_isolate: prolog: child exited %d",
                        WEXITSTATUS(status));
        unlink(nspath);
        return -1;
    }

    slurm_info("netns_isolate: created %s", nspath);
    return 0;
}


/* ===========================================================================
 * TASK INIT - runs as root in the forked task child, before exec()
 *
 * Enters the namespace created by the prolog. Because this runs post-fork
 * and pre-exec, it affects the task process only - slurmstepd is unaffected.
 * Fires for both sbatch job steps and interactive srun sessions.
 * ========================================================================= */
int slurm_spank_task_init_privileged(spank_t sp, int ac, char **av)
{
    char nspath[PATH_MAX];
    int fd;
    uid_t uid;

    (void)ac; (void)av;

    if (spank_context() != S_CTX_REMOTE)
        return 0;

    uid = getuid();
    if (uid != 0) {
        slurm_error("netns_isolate: task_init requires root (uid=%u)", uid);
        return -1;
    }

    if (get_ns_path(sp, nspath, sizeof(nspath), "task_init") < 0)
        return -1;

    fd = open(nspath, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT)
            slurm_error("netns_isolate: task_init: %s not found - "
                        "did the prolog succeed?", nspath);
        else
            slurm_error("netns_isolate: task_init: open(%s): %m", nspath);
        return -1;
    }

    /*
     * Enter the namespace. After this, the task process and everything it
     * exec's will have only loopback - no external interfaces, no routes out.
     */
    if (setns(fd, CLONE_NEWNET) < 0) {
        slurm_error("netns_isolate: task_init: setns(%s): %m", nspath);
        close(fd);
        return -1;
    }

    close(fd);
    slurm_verbose("netns_isolate: task entered %s", nspath);
    return 0;
}


/* ===========================================================================
 * EPILOG - runs as root on the compute node after all tasks have completed
 *
 * Destroys the namespace by unmounting the bind-mount and removing the file.
 * Always returns 0 to avoid blocking job accounting.
 * ========================================================================= */
int slurm_spank_job_epilog(spank_t sp, int ac, char **av)
{
    char nspath[PATH_MAX];
    int failed = 0;
    uid_t uid;

    (void)ac; (void)av;

    if (spank_context() != S_CTX_REMOTE)
        return 0;

    uid = getuid();
    if (uid != 0) {
        slurm_error("netns_isolate: epilog requires root (uid=%u)", uid);
        return 0; /* Don't block epilog */
    }

    if (get_ns_path(sp, nspath, sizeof(nspath), "epilog") < 0)
        return 0;

    /*
     * Lazily unmount the bind-mount. MNT_DETACH detaches immediately but
     * defers freeing the namespace until all processes inside have exited.
     * ENOENT/EINVAL mean it's already gone - not an error.
     */
    if (umount2(nspath, MNT_DETACH) < 0 && errno != ENOENT && errno != EINVAL) {
        slurm_error("netns_isolate: epilog: umount2(%s): %m", nspath);
        failed = 1;
    }

    if (unlink(nspath) < 0 && errno != ENOENT) {
        slurm_error("netns_isolate: epilog: unlink(%s): %m", nspath);
        failed = 1;
    }

    if (failed)
        slurm_error("netns_isolate: epilog: partial cleanup of %s - "
                    "manual intervention may be required", nspath);
    else
        slurm_verbose("netns_isolate: deleted %s", nspath);

    return 0; /* Always succeed - don't block job accounting */
}
