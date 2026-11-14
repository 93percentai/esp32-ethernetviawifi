#include "form_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char value[64];
    assert(form_get("ssid=a%26b+%2B%25&pass=12345678", "ssid", value, sizeof(value)));
    assert(!strcmp(value, "a&b +%"));
    assert(form_get("pass=&custom=", "pass", value, sizeof(value)) && !value[0]);
    assert(!form_get("pass=bad%", "pass", value, sizeof(value)));
    assert(!form_get("pass=bad%2", "pass", value, sizeof(value)));
    assert(!form_get("pass=bad%XY", "pass", value, sizeof(value)));
    assert(!form_get("pass=abc%00def", "pass", value, sizeof(value)));
    assert(!form_get("pass=a&pass=b", "pass", value, sizeof(value)));
    assert(!form_get("password=a", "pass", value, sizeof(value)));
    assert(!form_get("other=x&password=abc", "pass", value, sizeof(value)));
    char ssid[33];
    assert(form_get("ssid=12345678901234567890123456789012", "ssid", ssid, sizeof(ssid)));
    assert(strlen(ssid) == 32);
    assert(!form_get("ssid=123456789012345678901234567890123", "ssid", ssid, sizeof(ssid)));
    char escaped[200];
    html_escape("<script>&\"'", escaped, sizeof(escaped));
    assert(!strcmp(escaped, "&lt;script&gt;&amp;&quot;&#39;"));
    memset(ssid, '"', 32); ssid[32] = 0;
    assert(html_escape(ssid, escaped, sizeof(escaped)) == 192);
    char tiny[6];
    assert(html_escape("&&", tiny, sizeof(tiny)) == 5);
    assert(!strcmp(tiny, "&amp;"));
    puts("Wi-Fi form validation and HTML escaping passed");
}
