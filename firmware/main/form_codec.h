#pragma once
#include <stdbool.h>
#include <stddef.h>
/* Reject malformed escapes, NUL, duplicates and overlong decoded values. */
bool form_get(const char *body, const char *key, char *out, size_t capacity);
size_t html_escape(const char *in, char *out, size_t capacity);
