/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gc_update_version_compare: release ordering, including prereleases of the
 * same x.y.z (SemVer 2.0.0 section 11). */
#include "gc_update_policy.h"

#include <stdio.h>

#define PARSE_ERROR 9

static int failures;

static void expect(const char *left, const char *right, int want)
{
    int got = 99;
    const int rc = gc_update_version_compare(left, right, &got);
    if (want == PARSE_ERROR) {
        if (rc == 0) {
            printf("FAIL %s vs %s: expected a parse error, got %d\n", left, right, got);
            ++failures;
        }
        return;
    }
    if (rc != 0 || got != want) {
        printf("FAIL %s vs %s: rc=%d got=%d want=%d\n", left, right, rc, got, want);
        ++failures;
    }
}

int main(void)
{
    /* Ordering that was already right. */
    expect("v2.2.5", "2.2.5", 0);
    expect("v2.3.0", "2.2.9", 1);
    expect("2.10.0", "2.9.0", 1);
    expect("v2.2.5", "2.2.5-beta", 1);
    expect("2.2.5-beta", "2.2.5", -1);
    expect("v2.2.6-beta", "2.2.5", 1);

    /* Two prereleases of one x.y.z used to compare equal, so a newer beta was
     * never offered to someone on the previous one. */
    expect("v2.2.5-beta.2", "2.2.5-beta", 1);
    expect("2.2.5-beta", "2.2.5-beta.2", -1);
    expect("v2.2.5-rc1", "2.2.5-beta", 1);
    expect("v2.2.5-beta", "2.2.5-beta", 0);

    /* The precedence chain from SemVer 2.0.0 section 11.4, every pair. */
    static const char *chain[] = {
        "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
        "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0",
    };
    const int links = (int)(sizeof(chain) / sizeof(chain[0]));
    for (int i = 0; i < links; ++i)
        for (int j = 0; j < links; ++j)
            expect(chain[i], chain[j], i < j ? -1 : i > j ? 1 : 0);

    /* Build metadata does not affect precedence. */
    expect("v2.2.5+build7", "2.2.5", 0);
    expect("v2.2.5-beta.2+x", "2.2.5-beta.2", 0);

    /* Not versions. */
    expect("2.2", "2.2.0", PARSE_ERROR);
    expect("2.2.5-", "2.2.5", PARSE_ERROR);
    expect("2.2.5x", "2.2.5", PARSE_ERROR);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("version compare: all cases passed\n");
    return 0;
}
