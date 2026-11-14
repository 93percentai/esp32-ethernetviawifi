#pragma once
#include <stddef.h>
#include <stdint.h>
/* Caller serializes writes and snapshots. No allocation or logging inside. */
typedef struct {
    char *data;
    size_t capacity, used, next;
    uint64_t overwritten;
} log_buffer_t;
void log_buffer_init(log_buffer_t *buffer, char *storage, size_t capacity);
void log_buffer_append(log_buffer_t *buffer, const char *data, size_t size);
size_t log_buffer_copy(const log_buffer_t *buffer, char *out, size_t capacity);
