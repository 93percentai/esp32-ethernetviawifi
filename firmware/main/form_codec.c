#include "form_codec.h"
#include <string.h>

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool form_get(const char *body, const char *key, char *out, size_t capacity)
{
    if (!body || !key || !out || !capacity) return false;
    out[0] = 0;
    bool found = false;
    for (const char *p = body; *p;) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        const char *eq = memchr(p, '=', end - p);
        if (eq && (size_t)(eq - p) == strlen(key) && !memcmp(p, key, eq - p)) {
            if (found) return false;
            found = true;
            size_t n = 0;
            for (const char *v = eq + 1; v < end; ++v) {
                unsigned char c = *v;
                if (c == '+') c = ' ';
                else if (c == '%') {
                    if (end - v < 3 || hex(v[1]) < 0 || hex(v[2]) < 0) return false;
                    c = hex(v[1]) * 16 + hex(v[2]);
                    v += 2;
                }
                if (!c || n + 1 >= capacity) return false;
                out[n++] = c;
            }
            out[n] = 0;
        }
        p = *end ? end + 1 : end;
    }
    return found;
}

size_t html_escape(const char *in, char *out, size_t capacity)
{
    size_t n = 0;
    if (!capacity) return 0;
    for (; *in; ++in) {
        const char *r = NULL;
        switch (*in) {
        case '&': r = "&amp;"; break;
        case '<': r = "&lt;"; break;
        case '>': r = "&gt;"; break;
        case '"': r = "&quot;"; break;
        case '\'': r = "&#39;"; break;
        }
        size_t len = r ? strlen(r) : 1;
        if (n + len >= capacity) break;
        memcpy(out + n, r ? r : in, len);
        n += len;
    }
    out[n] = 0;
    return n;
}
