#pragma once
#include <stdbool.h>
#include <stddef.h>
bool url_form_get(const char *body, const char *key, char *output, size_t size);
void html_escape(const char *src, char *dst, size_t size);
