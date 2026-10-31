#include "url_codec.h"
#include <string.h>
#include <stdio.h>
static int hexdigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool url_form_get(const char *body, const char *key, char *output, size_t size)
{
    if (!body || !key || !output || !size) return false;
    size_t kl = strlen(key);
    const char *p = body;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq && (size_t)(eq-p) == kl && !memcmp(key, p, kl)) {
            size_t n = 0;
            for (const char *q = eq + 1; q < end; ++q) {
                unsigned char c = (unsigned char)*q;
                if (c == '+') c = ' ';
                else if (c == '%' && q + 2 < end && hexdigit(q[1]) >= 0 && hexdigit(q[2]) >= 0) {
                    c = (unsigned char)((hexdigit(q[1]) << 4) | hexdigit(q[2]));
                    q += 2;
                }
                if (c == 0) continue;
                if (n + 1 < size) output[n++] = c;
            }
            output[n] = 0;
            return true;
        }
        if (!*end) break;
        p = end + 1;
    }
    return false;
}
void html_escape(const char *src, char *dst, size_t size)
{
    if (!dst || !size) return;
    size_t n = 0;
    const char *s = src ? src : "";
    while (*s && n + 1 < size) {
        const char *r = NULL;
        switch (*s) {
            case '&': r = "&amp;"; break;
            case '"': r = "&quot;"; break;
            case '<': r = "&lt;"; break;
            case '>': r = "&gt;"; break;
            case '\'': r = "&#39;"; break;
        }
        if (r) {
            size_t l = strlen(r);
            if (n + l >= size) break;
            memcpy(dst + n, r, l); n += l;
        } else dst[n++] = *s;
        ++s;
    }
    dst[n] = 0;
}
