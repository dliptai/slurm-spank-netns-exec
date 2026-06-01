/*
 * Integration test for netns_spank plugin.
 *
 * SETUP REQUIRED:
 *   Before running this test, create a network namespace:
 *     sudo ip netns add test-ns
 *     sudo ip netns exec test-ns ip link set lo up
 *
 * USAGE:
 *   sudo SPANK_CONTEXT=1 SLURM_JOB_PARTITION=test-partition ./test \
 *     partition=test-partition netns=/var/run/netns/test-ns
 *
 * NOTES:
 *   - Must run as root to actually enter namespaces
 *   - Tests iproute_bind_mounts() with dynamic mount namespace creation
 *   - Tests configuration parsing and error handling
 */
#include "netns_common.h"

spank_context_t spank_context(void) {
    const char *env_val = getenv("SPANK_CONTEXT");
    return (env_val && strcmp(env_val, "1") == 0) ? S_CTX_REMOTE : S_CTX_LOCAL;
}

spank_err_t spank_getenv(spank_t sp, const char *var, char *buf, int len) {
    (void)sp;

    if (!var || !buf || len <= 0)
        return ESPANK_BAD_ARG;

    const char *val = getenv(var);
    if (!val)
        return ESPANK_ENV_NOEXIST;

    size_t n = strnlen(val, len - 1);
    memcpy(buf, val, n);
    buf[n] = '\0';
    return ESPANK_SUCCESS;
}

static const char *error_name(int rc) {
    switch (rc) {
        case 0: return "SUCCESS";
        case RC_MISSING_CONFIG: return "RC_MISSING_CONFIG";
        case RC_UNKNOWN_OPT: return "RC_UNKNOWN_OPT";
        case RC_NOT_REMOTE_CTX: return "RC_NOT_REMOTE_CTX";
        case RC_GETENV_FAIL: return "RC_GETENV_FAIL";
        case RC_WRONG_PARTITION: return "RC_WRONG_PARTITION";
        case RC_NO_NAMESPACE: return "RC_NO_NAMESPACE";
        case RC_NAMESPACE_OPEN_FAIL: return "RC_NAMESPACE_OPEN_FAIL";
        case RC_NAMESPACE_NOT_ROOT: return "RC_NAMESPACE_NOT_ROOT";
        case RC_SETNS_FAIL: return "RC_SETNS_FAIL";
        case RC_UNKNOWN_NS_TYPE: return "RC_UNKNOWN_NS_TYPE";
        case RC_UNSHARE_FAIL: return "RC_UNSHARE_FAIL";
        case RC_MOUNT_RSLAVE_FAIL: return "RC_MOUNT_RSLAVE_FAIL";
        case RC_MOUNT_SYS_FAIL: return "RC_MOUNT_SYS_FAIL";
        case RC_BIND_MOUNTS_FAIL: return "RC_BIND_MOUNTS_FAIL";
        default: return "UNKNOWN";
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage example: %s partition=test-partition netns=/var/run/netns/test-ns\n", argv[0]);
        return 255;
    }

    int rc = slurm_spank_init(NULL, argc - 1, argv + 1);
    if (rc != 0) {
        fprintf(stderr, "slurm_spank_init failed: %s (%d)\n", error_name(rc), rc);
        return rc;
    }

    rc = slurm_spank_task_init_privileged(NULL, argc - 1, argv + 1);
    printf("result: %s (%d)\n", error_name(rc), rc);
    return rc;
}
