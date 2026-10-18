#ifndef STUB_OSDEP_H
#define STUB_OSDEP_H
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <inttypes.h>
typedef uint64_t hwaddr;
#define HWADDR_PRIx PRIx64
typedef struct Error Error;
static inline void set_bit(unsigned n, unsigned long *a){ a[n/64] |= 1UL<<(n%64); }
#endif
