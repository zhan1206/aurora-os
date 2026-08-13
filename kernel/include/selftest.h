/* FIXED (v4.4.3): P2-2.2 — Selftest result tracking header */
#ifndef SELFTEST_H
#define SELFTEST_H

struct selftest_result {
    char name[64];
    int passed;
};

void selftest_record_result(const char *name, int passed);
void selftest_get_summary(int *total, int *passed, int *failed);
const struct selftest_result *selftest_get_result(int index);
int selftest_get_result_count(void);

#endif /* SELFTEST_H */