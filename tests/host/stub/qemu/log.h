#ifndef STUB_LOG_H
#define STUB_LOG_H
#define LOG_GUEST_ERROR 1
extern int stub_log_count;
#define qemu_log_mask(m, ...) do { stub_log_count++; if (getenv("STUB_VERBOSE")) fprintf(stderr, __VA_ARGS__); } while (0)
#endif
