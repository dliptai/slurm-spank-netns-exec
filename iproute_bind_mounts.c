/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Derived from iproute2 namespace.c
 *
 * https://github.com/iproute2/iproute2/blob/62d47c2dbc0eaecdd20c0e19406067488025e92e/lib/namespace.c
 *
 * Original source:
 *   iproute2: namespace.c
 *
 * Modifications:
 *   - Changed logging to use custom logging functions
 *   - Changed return codes to use plugin-specific error codes
 *   - Added check to ensure namespace file is owned by root
 *   - Added O_NOFOLLOW to open() to prevent symlink attacks
 */

#include "netns_common.h"


int bind_etc(const char *name){
    char etc_netns_path[sizeof(NETNS_ETC_DIR) + NAME_MAX];
    char netns_name[PATH_MAX];
    char etc_name[PATH_MAX];
    struct dirent *entry;
    DIR *dir;

    if (strlen(name) >= NAME_MAX) {
        log_error("Namespace name too long");
        return RC_NAMESPACE_TOO_LONG;
    }

    snprintf(etc_netns_path, sizeof(etc_netns_path), "%s/%s", NETNS_ETC_DIR, name);
    dir = opendir(etc_netns_path);
    if (!dir) {
        log_verbose("No config files to bind mount. '%s' doesn't exist", etc_netns_path);
        return 0;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0)
            continue;
        if (strcmp(entry->d_name, "..") == 0)
            continue;
        snprintf(netns_name, sizeof(netns_name), "%s/%s", etc_netns_path, entry->d_name);
        snprintf(etc_name, sizeof(etc_name), "/etc/%s", entry->d_name);
        if (mount(netns_name, etc_name, "none", MS_BIND, NULL) < 0) {
            log_error("Bind %s -> %s failed: %m", netns_name, etc_name);
            closedir(dir);
            return RC_BIND_MOUNTS_FAIL;
        } else {
            log_verbose("Bind mounted '%s' -> '%s'", netns_name, etc_name);
        }
    }
    closedir(dir);
    return 0;
}

int netns_switch(const char *name)
{
    char net_path[PATH_MAX];
    int netns;
    unsigned long mountflags = 0;
    struct statvfs fsstat;

    /*
     * Open the namespace file with secure flags:
     *   O_RDONLY       - read-only (we only need to pass it to setns)
     *   O_CLOEXEC      - close on execve() to prevent leaking the fd to the job
     *   O_NOFOLLOW     - fail if ns_path is a symlink (prevents symlink attacks)
     */
    snprintf(net_path, sizeof(net_path), "%s/%s", NETNS_RUN_DIR, name);
    netns = open(net_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (netns < 0) {
        log_error("Cannot open network namespace \"%s\": %m", name);
        return RC_NAMESPACE_OPEN_FAIL;
    }

    /* Ensure namespace is owned by root */
    struct stat st;
    if (fstat(netns, &st) < 0 || st.st_uid != 0) {
        log_error("%s namespace not owned by root!", name);
        close(netns);
        return RC_NAMESPACE_NOT_ROOT;
    }

    if (setns(netns, CLONE_NEWNET) < 0) {
        log_error("setting the network namespace \"%s\" failed: %m", name);
        close(netns);
        return RC_SETNS_FAIL;
    }
    close(netns);

    log_verbose("Task entered network namespace '%s'", name);

    if (unshare(CLONE_NEWNS) < 0) {
        log_error("unshare() failed: %m");
        return RC_UNSHARE_FAIL;
    }
    /* Don't let any mounts propagate back to the parent */
    if (mount("", "/", "none", MS_SLAVE | MS_REC, NULL)) {
        log_error("\"mount --make-rslave /\" failed: %m");
        return RC_MOUNT_RSLAVE_FAIL;
    }

    /* Mount a version of /sys that describes the network namespace */

    if (umount2("/sys", MNT_DETACH) < 0) {
        /* If this fails, perhaps there wasn't a sysfs instance mounted. Good. */
        if (statvfs("/sys", &fsstat) == 0) {
            /* We couldn't umount the sysfs, we'll attempt to overlay it.
             * A read-only instance can't be shadowed with a read-write one. */
            if (fsstat.f_flag & ST_RDONLY)
                mountflags = MS_RDONLY;
        }
    }
    if (mount(name, "/sys", "sysfs", mountflags, NULL) < 0) {
        log_error("Mount of /sys failed: %m");
        return RC_MOUNT_SYS_FAIL;
    }

    /* Setup bind mounts for config files in /etc */
    return bind_etc(name);
}
