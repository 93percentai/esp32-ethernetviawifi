#pragma once
#include <stddef.h>
#include <stdint.h>
void diagnostics_init(void);
size_t diagnostics_log_tail(char *out,size_t capacity,uint64_t *overwritten,uint64_t *truncated);
void diagnostics_serial_status(void);
