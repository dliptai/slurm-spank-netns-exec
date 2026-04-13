/*
 * Simple test for netns_spank plugin initialization
 */

#include <stdlib.h>
#include <stdio.h>
#include <slurm/spank.h>
#include <string.h>
#include "netns_spank.h"

/* Mock SPANK API functions */
spank_context_t spank_context (void) {
    // use env variable to override context for testing
    const char *env_val = getenv("SPANK_CONTEXT");
    // if variable is 1, return S_CTX_REMOTE, else return 0
    if (env_val && strcmp(env_val, "1") == 0) {
        return S_CTX_REMOTE;
    }
    return S_CTX_REMOTE + 1;  // Return a non-remote context by default for testing
}
spank_err_t spank_getenv(spank_t sp, const char *var, char *buf, int len) {

    // use getenv to get var from environment
    const char *env_val = getenv(var);
    if (env_val) {
        strncpy(buf, env_val, len - 1);
        buf[len - 1] = '\0';
        return ESPANK_SUCCESS;
    }
    return ESPANK_SUCCESS;;
}

/* Print test information */
void print_test(const char *test_name, int expected_rc) {
    printf("\n[TESTING: %s]\n", test_name);
}

/* Compare the actual return code to the expected return code and print pass/fail */
int compare_rc(const char *test_name, int rc, int expected_rc) {
    if (rc == expected_rc) {
        printf("[PASS]\n");
        return 0;
    } else {
        printf("[FAIL] %s (expected = %d, got = %d)\n",
                test_name, expected_rc, rc);
        return 1;
    }
}

/*
 * Run a single test case
 */
int test_slurm_spank_init(const char *test_name, char **av, int ac, int expected_rc)
{
    print_test(test_name, expected_rc);
    int rc = slurm_spank_init(NULL, ac, av);
    return compare_rc(test_name, rc, expected_rc);
}

/* Run all slurm_spank_init tests */
int test_all_slurm_spank_init(void)
{
    int failures = 0;

    printf("\n>>> Running netns_spank_init tests\n");

    failures += test_slurm_spank_init("Valid config",
        (char *[]) {
            "partition=test",
            "netns=/var/run/netns/test"
        }, 2, 0);

    failures += test_slurm_spank_init("Missing config", (char *[]) {NULL}, 0, RC_MISSING_CONFIG);

    failures += test_slurm_spank_init("Unknown option", (char *[]) {
        "partition=test",
        "netns=/var/run/netns/test",
        "foo=bar"
    }, 3, RC_UNKNOWN_OPT);

    printf("\nResult: %s (%d failures)\n",
            failures == 0 ? "PASS" : "FAIL",
            failures);

    return failures;
}

int test_slurm_spank_task_init_privileged(const char *test_name, char **av, int expected_rc)
{
    int ac = 2;
    print_test(test_name, expected_rc);
    int rc = slurm_spank_task_init_privileged(NULL, ac, av);
    return compare_rc(test_name, rc, expected_rc);
}

int test_all_slurm_spank_task_init_privileged(void)
{
    printf("\n>>> Running netns_spank_task_init_privileged tests\n");

    // Set environment variable to simulate remote context for the test
    setenv("SPANK_CONTEXT", "1", 1);  // Set back to remote context for remaining tests

    // Set environment variable to match the partition for the test
    setenv("SLURM_JOB_PARTITION", "test", 1);
    int failures = 0;
    char *av[] = {
        "partition=test",
        "netns=/var/run/netns/test"
    };
    slurm_spank_init(NULL, 2, av); // Initialize config for the test

    // Expect failure due to no kernel support for namespaces in the test environment
    failures += test_slurm_spank_task_init_privileged("setns fail", av, -9);

    setenv("SPANK_CONTEXT", "0", 1);  // Ensure spank_context() returns non-remote context
    test_slurm_spank_task_init_privileged("Not remote context", av, RC_NOT_REMOTE_CTX);
    setenv("SPANK_CONTEXT", "1", 1);  // Set back to remote context for remaining tests

    setenv("SLURM_JOB_PARTITION", "bad_parition_name", 1);
    test_slurm_spank_task_init_privileged("Partition mismatch", av, RC_WRONG_PARTITION);
    setenv("SLURM_JOB_PARTITION", "not_test", 1);


    printf("\nResult: %s (%d failures)\n",
        failures == 0 ? "PASS" : "FAIL",
        failures);

    return failures;
}

int main(void)
{
    int total_failures = 0;
    total_failures += test_all_slurm_spank_init();
    total_failures += test_all_slurm_spank_task_init_privileged();

    printf("\n==== Summary =============================");
    printf("\nOverall result: %s (%d total failures)\n",
        total_failures == 0 ? "PASS" : "FAIL",
        total_failures);
    printf("==========================================\n");

    return total_failures;
}
