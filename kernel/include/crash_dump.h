/* FIXED (v4.4.3): P2-2.3 — Crash dump persistence header */
#ifndef CRASH_DUMP_H
#define CRASH_DUMP_H

void crash_dump_write_to_disk(const char *reason);

#endif /* CRASH_DUMP_H */