#include "../main/url_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_decode(void)
{
    char a[48];
    assert(url_form_get("ssid=Mein+WLAN&name=A%26B&pass=p%2Bq", "ssid", a, sizeof a));
    assert(strcmp(a, "Mein WLAN") == 0);
    assert(url_form_get("ssid=Mein+WLAN&name=A%26B&pass=p%2Bq", "name", a, sizeof a));
    assert(strcmp(a, "A&B") == 0);
    assert(url_form_get("ssid=Mein+WLAN&name=A%26B&pass=p%2Bq", "pass", a, sizeof a));
    assert(strcmp(a, "p+q") == 0);
    assert(!url_form_get("ab=x", "a", a, sizeof a));
    assert(!url_form_get("ssid=x", "ssid", a, 0));
    assert(url_form_get("ssid=%3Cscript%3E", "ssid", a, sizeof a));
    assert(strcmp(a, "<script>") == 0);
    assert(url_form_get("ssid=abcdef", "ssid", a, 3));
    assert(strcmp(a, "ab") == 0);
}

static void test_html(void)
{
    char a[128];
    html_escape("<a href=\"x&y\">'z'</a>", a, sizeof a);
    assert(strcmp(a, "&lt;a href=&quot;x&amp;y&quot;&gt;&#39;z&#39;&lt;/a&gt;") == 0);
    html_escape("x", a, 1);
    assert(strcmp(a, "") == 0);
}

int main(void)
{
    test_decode();
    test_html();
    puts("OK: URL-Formulardecodierung und HTML-Escaping");
    return 0;
}
