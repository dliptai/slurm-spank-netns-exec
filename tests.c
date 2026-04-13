/*
 * Simple test for netns_spank plugin initialization
 */
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <slurm/spank.h>
#include <string.h>
#include "netns_spank.h"

/*
* Mock function.
* Use env variable to override context for testing.
* Returns S_CTX_REMOTE if SPANK_CONTEXT=1, else returns S_CTX_LOCAL.
*/
spank_context_t spank_context (void) {
    const char *env_val = getenv("SPANK_CONTEXT");
    if (env_val && strcmp(env_val, "1") == 0) {
        return S_CTX_REMOTE;
    }
    return S_CTX_LOCAL;
}

/*
* Mock function.
* Place a copy of environment variable "var" from the job's environment
* into buffer "buf" of size "len."
*
* Returns ESPANK_SUCCESS on success, o/w spank_err_t on failure:
*   ESPANK_BAD_ARG      = spank handle invalid or len < 0.
*   ESPANK_ENV_NOEXIST  = environment variable doesn't exist in job's env.
*   ESPANK_NOSPACE      = buffer too small, truncation occurred.
*   ESPANK_NOT_REMOTE   = not called in remote context (i.e. from slurmd).
*
* For testing, we will use the actual environment variables of the test process.
*/
spank_err_t spank_getenv(spank_t sp, const char *var, char *buf, int len) {
    (void)sp;  // unused in test fake

    // Validate arguments
    if (!var || !buf || len <= 0)
        return ESPANK_BAD_ARG;

    const char *val = getenv(var);
    if (!val)
        return ESPANK_ENV_NOEXIST;

    size_t n = strlen(val);

    // Always null-terminate if buffer is usable
    if (len > 0)
        buf[0] = '\0';

    if ((int)n >= len) {
        // Truncate
        memcpy(buf, val, len - 1);
        buf[len - 1] = '\0';
        return ESPANK_NOSPACE;
    }

    memcpy(buf, val, n + 1);  // include null terminator
    return ESPANK_SUCCESS;
}

int run_test(int argc, char *argv[])
{
     // Initialize config
    int rc = slurm_spank_init(NULL, argc, argv);
    if (rc != 0) {
        fprintf(stderr, "Failed to initialize slurm_spank. rc = %d\n", rc);
        return rc;
    }
    // Run plugin
    return slurm_spank_task_init_privileged(NULL, argc, argv);
}


int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Missing test configuration.\n");
        fprintf(stderr, "usage:\n");
        fprintf(stderr, "    %s key1=value key2=value ...\n", argv[0]);
        return 255;
    }

    return run_test(argc - 1, argv + 1);
}
