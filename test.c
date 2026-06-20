/* SPDX-License-Identifier: GPL-2.0-or-later */
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

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage example: %s partition=test-partition netns=/var/run/netns/test-ns\n", argv[0]);
        return 255;
    }

    int rc = slurm_spank_init(NULL, argc - 1, argv + 1);
    if (rc != 0) {
        fprintf(stderr, "slurm_spank_init failed: %d\n", rc);
        return rc;
    }

    rc = slurm_spank_task_init_privileged(NULL, argc - 1, argv + 1);
    return rc;
}
