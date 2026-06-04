#ifndef NETNS_COMMON_H
#define NETNS_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* Plugin identification */
#define PLUGIN_NAME "netns_spank"

/* Common constants */
#define NETNS_ETC_DIR "/etc/netns"
#define NETNS_RUN_DIR "/var/run/netns"
#define PARTNAME_MAX 64
#define NS_NAME_MAX 16

/* Return codes for error paths */
#define RC_MISSING_CONFIG       1
#define RC_UNKNOWN_OPT          2
#define RC_NOT_REMOTE_CTX       3
#define RC_GETENV_FAIL          4
#define RC_WRONG_PARTITION      5
#define RC_NO_NAMESPACE         6
#define RC_NAMESPACE_OPEN_FAIL  7
#define RC_NAMESPACE_NOT_ROOT   8
#define RC_SETNS_FAIL           9
#define RC_UNKNOWN_NS_TYPE      10
#define RC_UNSHARE_FAIL         11
#define RC_MOUNT_RSLAVE_FAIL    12
#define RC_MOUNT_SYS_FAIL       13
#define RC_BIND_MOUNTS_FAIL     14
#define RC_NAMESPACE_TOO_LONG   15

/* Common includes */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <slurm/spank.h>

/* Logging macros */
#ifdef TEST
#define PLUGIN_VERBOSE(...) fprintf(stdout, __VA_ARGS__), fprintf(stdout, "\n")
#define PLUGIN_ERROR(...) fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\n")
#else
#define PLUGIN_VERBOSE(...) slurm_verbose(__VA_ARGS__)
#define PLUGIN_ERROR(...) slurm_error(__VA_ARGS__)
#endif

#define log_verbose(...) PLUGIN_VERBOSE(PLUGIN_NAME ": " __VA_ARGS__)
#define log_error(...)   PLUGIN_ERROR(PLUGIN_NAME ": " __VA_ARGS__)

/* Function declarations */
int netns_switch(const char *name);
int slurm_spank_task_init_privileged(spank_t sp, int ac, char **av);

#endif
