#ifndef LLCCPV_UTIL_H
#define LLCCPV_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#define LOG_INFO(...)  do { fprintf(stderr, "[llccpv] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define LOG_WARN(...)  do { fprintf(stderr, "[llccpv] WARN: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define LOG_ERROR(...) do { fprintf(stderr, "[llccpv] ERROR: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)

struct frame_info {
    void *data;
    size_t size;
    int width;
    int height;
    uint32_t pixfmt;
    int buf_index;
};

#endif
