#include "log_buffer.h"
#include <string.h>
void log_buffer_init(log_buffer_t *buffer, char *storage, size_t capacity)
{
    *buffer = (log_buffer_t){.data = storage, .capacity = capacity};
}
void log_buffer_append(log_buffer_t *buffer, const char *data, size_t size)
{
    if (!buffer->capacity || !size) return;
    size_t free_bytes = buffer->capacity - buffer->used;
    if (size > free_bytes) buffer->overwritten += size - free_bytes;
    if (size >= buffer->capacity) {
        memcpy(buffer->data, data + size - buffer->capacity, buffer->capacity);
        buffer->used = buffer->capacity;
        buffer->next = 0;
        return;
    }
    size_t first = buffer->capacity - buffer->next;
    if (first > size) first = size;
    memcpy(buffer->data + buffer->next, data, first);
    memcpy(buffer->data, data + first, size - first);
    buffer->next = (buffer->next + size) % buffer->capacity;
    buffer->used += size;
    if (buffer->used > buffer->capacity) buffer->used = buffer->capacity;
}
size_t log_buffer_copy(const log_buffer_t *buffer, char *out, size_t capacity)
{
    if (!capacity) return 0;
    size_t count = buffer->used;
    if (count >= capacity) count = capacity - 1;
    if (count) {
        /* A smaller destination gets the most recent bytes. */
        size_t start = (buffer->next + buffer->capacity - count) % buffer->capacity;
        size_t first = buffer->capacity - start;
        if (first > count) first = count;
        memcpy(out, buffer->data + start, first);
        memcpy(out + first, buffer->data, count - first);
    }
    out[count] = 0;
    return count;
}
