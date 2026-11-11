#ifndef CORE_UTIL_H
#define CORE_UTIL_H

#include <stdbool.h>
#include <stdint.h>

#define KiB(x) ((uint64_t)(x) << 10)
#define MiB(x) ((uint64_t)(x) << 20)
#define GiB(x) ((uint64_t)(x) << 30)
#define PAGE_SIZE 4096u

static inline uint16_t rd16(const void *p) { uint16_t v; __builtin_memcpy(&v, p, 2); return v; }
static inline uint32_t rd32(const void *p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
static inline uint64_t rd64(const void *p) { uint64_t v; __builtin_memcpy(&v, p, 8); return v; }

static inline uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
static inline uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
static inline bool is_pow2(uint64_t v) { return v && !(v & (v - 1)); }

typedef bool (*phys_range_ok_fn)(uint64_t addr, uint64_t len);

#endif
