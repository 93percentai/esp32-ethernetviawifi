#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "log_buffer.h"

/* Compare every wrap position and snapshot size against a chronological history. */
int main(void)
{
    char storage[31], history[8192], out[40];
    for (size_t capacity = 0; capacity <= sizeof(storage); ++capacity) {
        log_buffer_t buffer;
        log_buffer_init(&buffer, storage, capacity);
        size_t total = 0;
        unsigned seed = 42;
        for (unsigned write = 0; write < 100; ++write) {
            seed = seed * 1664525u + 1013904223u;
            size_t length = seed % 67; /* Includes writes larger than the ring. */
            char fragment[67];
            for (size_t i = 0; i < length; ++i) fragment[i] = (char)('a' + (total + i) % 26);
            memcpy(history + total, fragment, length);
            total += length;
            log_buffer_append(&buffer, fragment, length);
            size_t retained = total < capacity ? total : capacity;
            assert(buffer.used == retained);
            assert(buffer.overwritten == (capacity ? total - retained : 0));
            for (size_t destination = 0; destination <= sizeof(out); ++destination) {
                memset(out, '#', sizeof(out));
                size_t wanted = destination ? destination - 1 : 0;
                if (wanted > retained) wanted = retained;
                assert(log_buffer_copy(&buffer, out, destination) == wanted);
                assert(memcmp(out, history + total - wanted, wanted) == 0);
                if (destination) assert(out[wanted] == 0);
                else assert(out[0] == '#');
            }
        }
    }
    puts("Log ring tests passed");
    return 0;
}
