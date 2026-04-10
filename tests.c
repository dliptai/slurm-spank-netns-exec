/*
 * Simple test for netns_spank plugin initialization
 */

#include <stdio.h>
#include "netns_spank.h"

// /* Mock SPANK API functions not needed for this test */
int spank_context(void) { return -1; }
int spank_getenv(void *sp, const char *name, char *buf, int len) { return 0; }

/* Forward declare the real function */
int slurm_spank_init(void *sp, int ac, char **av);

/*
 * Run a single test case
 */
int test_slurm_spank_init(char **av, int ac, int expected_rc, const char *test_name)
{
    printf("\n>>> [%s] (expect = %d)\n", test_name, expected_rc);
    int rc = slurm_spank_init(NULL, ac, av);
    if (rc == expected_rc) {
        printf("[PASS]\n");
        return 0;
    } else {
        printf("[FAIL] %s (expected = %d  got = %d)\n",
               test_name, expected_rc, rc);
        return 1;
    }
}

int main(void)
{
    int failures = 0;

    printf("\n>>> Running netns_spank_init tests\n");

    char *av3[] = {
        "partition=test",
        "netns=/var/run/netns/test"
    };
    failures += test_slurm_spank_init(av3, 2, 0, "Valid config test");

    char *av1[] = { NULL };
    failures += test_slurm_spank_init(av1, 0, RC_MISSING_CONFIG, "Missing config test");

    char *av2[] = {
        "partition=test",
        "netns=/var/run/netns/test",
        "foo=bar"
    };
    failures += test_slurm_spank_init(av2, 3, RC_UNKNOWN_OPT, "Unknown option test");

    printf("\nResult: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);

    return failures;
}
