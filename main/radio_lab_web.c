#include "radio_lab_web.h"
#include "radio_stream.h"
#include "radio_ui.h"
#include "radio_diag.h"
#include "radio_wifi.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "url_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Static, self-contained HTML: no external JavaScript, images or CSS assets. */
static const char LAB_PAGE[] =
"<!doctype html>\n"
"<html lang=\"de\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>Küchenradio | Diagnoselabor</title>\n"
"<style>\n"
":root{color-scheme:dark;font-family:system-ui,Arial;background:#0b1422;color:#ecf5f6}\n"
"body{max-width:1000px;margin:0 auto;padding:18px 14px 60px}\n"
"h1{font-size:1.55rem;margin:10px 0}h2{font-size:1.12rem;color:#a5f3fc}\n"
"a{color:#67e8f9}p,small{color:#b2c6cf}\n"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(155px,1fr));gap:10px}\n"
".card,section{border:1px solid #294050;border-radius:12px;background:#132434;padding:13px;margin:12px 0}\n"
".kpi{font-size:1.35rem;font-weight:700;color:white;display:block}\n"
".caption{font-size:.79rem;color:#a5b7c3}\n"
"button,select{border:1px solid #467178;border-radius:8px;background:#0f766e;color:white;padding:11px;font-size:1rem;cursor:pointer}\n"
"select{background:#1f3849;width:100%}button.alt{background:#293f57}\n"
"button.warn{background:#9a3412}\n"
".row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}.row>*{flex:1}\n"
"label{display:block;margin:12px 0 7px}\n"
"input[type=range]{width:100%;accent-color:#2dd4bf}\n"
"canvas{display:block;width:100%;height:170px;background:#0b1725;border-radius:8px}\n"
".status{padding:8px;border-left:3px solid #14b8a6;margin-top:10px}\n"
"code{color:#a5f3fc}button:disabled{opacity:.5}\n"
"</style></head><body>\n"
"<p><a href=\"/\">← Sender konfigurieren</a> · <a href=\"/diagnose\">Textdiagnose</a> · <a href=\"/lab/csv\">CSV-Diagnose exportieren</a></p>\n"
"<h1>🔬 Küchenradio Diagnoselabor #65</h1>\n"
"<p>Messwerte werden jede Sekunde erfasst. Alle Test-Einstellungen sind <b>temporär</b> und gehen beim Neustart verloren.</p>\n"
"<div class=\"grid\">\n"
"<div class=\"card\"><small>Modus</small><span class=\"kpi\" id=\"modeName\">–</span></div>\n"
"<div class=\"card\"><small>PCM-Ring</small><span class=\"kpi\" id=\"pcmLevel\">–</span><span class=\"caption\" id=\"pcmRange\"></span></div>\n"
"<div class=\"card\"><small>PCM-Ring-Leerläufe</small><span class=\"kpi\" id=\"pcmUnder\">–</span></div>\n"
"<div class=\"card\"><small>Bei Touch (≤1 s)</small><span class=\"kpi\" id=\"touchUnder\">–</span></div>\n"
"<div class=\"card\"><small>I2S-Schreibfehler</small><span class=\"kpi\" id=\"i2sErrors\">–</span></div>\n"
"<div class=\"card\"><small>Längster Codec-Write</small><span class=\"kpi\" id=\"codecMax\">–</span></div>\n"
"<div class=\"card\"><small>Interner RAM frei</small><span class=\"kpi\" id=\"ram\">–</span></div>\n"
"<div class=\"card\"><small>Größter RAM-Block</small><span class=\"kpi\" id=\"largest\">–</span></div>\n"
"<div class=\"card\"><small>I2S-Stackreserve</small><span class=\"kpi\" id=\"stack\">–</span></div>\n"
"<div class=\"card\"><small>WLAN-Signal</small><span class=\"kpi\" id=\"rssi\">–</span></div>\n"
"</div>\n"
"<section>\n"
"<h2>Audiokette isolieren</h2>\n"
"<p>Beim Testton erfolgen keine HTTP-Lesevorgänge und keine MP3-Decodierung. Alle drei Modi nutzen denselben ES8311.</p>\n"
"<label for=\"mode\">Audiomodus</label>\n"
"<select id=\"mode\"><option value=\"0\">Internetradio (WLAN + Decoder + PCM + I2S)</option>\n"
"<option value=\"1\">1-kHz-Testton direkt an I2S (ohne PCM-Puffer)</option>\n"
"<option value=\"2\">1-kHz-Testton über PCM-Ring (ohne WLAN/Decoder)</option></select>\n"
"<div class=\"row\" style=\"margin-top:12px\"><button onclick=\"apply()\">Modus und Regler übernehmen</button>\n"
"<button class=\"alt\" onclick=\"resetSettings()\">Standardwerte einstellen</button></div>\n"
"<p class=\"caption\">Tonpegel bewusst reduziert. Beim Moduswechsel gibt es eine kurze Unterbrechung.</p>\n"
"</section>\n"
"<section>\n"
"<h2>Temporäre Tuning-Regler</h2>\n"
"<label for=\"prime\">PCM-Startfüllung: <b id=\"primeValue\">24 KB</b></label>\n"
"<input type=\"range\" id=\"prime\" min=\"8\" max=\"48\" step=\"4\" value=\"24\" oninput=\"document.getElementById('primeValue').textContent=this.value+' KB'\">\n"
"<label for=\"ui\">Informationsaktualisierung auf dem Gerät</label>\n"
"<select id=\"ui\"><option value=\"500\">Alle 500 ms</option><option value=\"1000\" selected>Alle 1000 ms</option><option value=\"2000\">Alle 2000 ms</option></select>\n"
"<p class=\"caption\">Dies verändert den 1-s-Status-Timer, nicht den RGB-Pixeltakt und nicht die LVGL-Renderfrequenz. PSRAM-Takt und Cachegröße sind Build-Einstellungen.</p>\n"
"</section>\n"
"<section>\n"
"<h2>PCM-Füllstand · letzte 120 Sekunden</h2><canvas id=\"pcmChart\"></canvas>\n"
"<p class=\"caption\">0 KB = PCM-Ring leer; das beweist nicht automatisch eine hörbare I2S-DMA-Unterbrechung.</p>\n"
"<h2>Freier interner RAM · letzte 120 Sekunden</h2><canvas id=\"ramChart\"></canvas>\n"
"</section>\n"
"<section>\n"
"<h2>Störung markieren</h2>\n"
"<p>Wenn du ein Knacken, eine Verzerrung oder Flackern beobachtest, drücke unmittelbar diesen Knopf. Der Zeitstempel erscheint im Diagnose-CSV.</p>\n"
"<div class=\"row\"><button class=\"warn\" onclick=\"mark()\">Gerade Ton-/Bildstörung gehört oder gesehen</button>\n"
"<button class=\"alt\" onclick=\"location.href='/lab/csv'\">CSV-Diagnose herunterladen</button></div>\n"
"<div id=\"message\" class=\"status\">Bereit</div>\n"
"</section>\n"
"<section><h2>Zusatzdiagnose</h2>\n"
"<div id=\"other\">Verbinde mit Gerät …</div>\n"
"<p class=\"caption\">Die Software misst Aufrufzeiten, PCM-Puffer und Task-Reserven. Direkte DAC-Stille, RGB-DMA-Unterläufe und reale elektrische Versorgungsspannung werden hier nicht hardwareseitig gemessen.</p></section>\n"
"<script>\n"
"const el=id=>document.getElementById(id);\n"
"let initial=false;\n"
"let busy=false;\n"
"async function send(path,data){\n"
" const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data)});\n"
" if(!r.ok)throw Error(await r.text());\n"
" return await r.text();\n"
"}\n"
"async function apply(){\n"
" if(busy)return;busy=true;\n"
" try{\n"
"  const q={mode:el('mode').value,prime:el('prime').value,ui:el('ui').value};\n"
"  el('message').textContent=await send('/lab/set',q);\n"
" }catch(e){el('message').textContent='Fehler: '+e.message}finally{busy=false}\n"
"}\n"
"async function resetSettings(){\n"
" el('mode').value='0';el('prime').value='24';el('ui').value='1000';\n"
" el('primeValue').textContent='24 KB';await apply();\n"
"}\n"
"async function mark(){\n"
" try{el('message').textContent=await send('/lab/mark',{})}\n"
" catch(e){el('message').textContent='Fehler: '+e.message}\n"
"}\n"
"function kb(n){return (n/1024).toFixed(1)+' KB'}\n"
"async function refresh(){\n"
" try{\n"
"  const r=await fetch('/lab/state',{cache:'no-store'});if(!r.ok)throw Error(r.status);\n"
"  const s=await r.json();\n"
"  const modes=['Radio','Testton direkt','Testton PCM'];\n"
"  el('modeName').textContent=modes[s.mode]||'–';\n"
"  el('pcmLevel').textContent=kb(s.pcm_bytes)+' / '+kb(s.pcm_capacity);\n"
"  el('pcmRange').textContent='Minimum '+kb(s.pcm_min)+' · Maximum '+kb(s.pcm_max);\n"
"  el('pcmUnder').textContent=s.pcm_underruns;\n"
"  el('touchUnder').textContent=s.touch_near_pcm;\n"
"  el('i2sErrors').textContent=s.i2s_errors;\n"
"  el('codecMax').textContent=s.codec_max+' ms';\n"
"  el('ram').textContent=kb(s.ram_free);\n"
"  el('largest').textContent=kb(s.ram_largest);\n"
"  el('stack').textContent=s.i2s_stack?kb(s.i2s_stack):'noch nicht erfasst';\n"
"  el('rssi').textContent=s.rssi===null?'–':s.rssi+' dBm';\n"
"  el('other').textContent='HTTP-Bytes: '+s.http_bytes+' · HTTP-FIFO: '+kb(s.fifo_bytes)+'/'+kb(s.fifo_capacity)+' · WLAN-Abbrüche: '+s.wifi_lost+' · Touch: '+s.touches+' · Markierungen: '+s.marks+' · Decoder max: '+s.decoder_max+' ms · I2S-Schreibaufrufe: '+s.i2s_calls+' · PCM+Touch-Leerläufe: '+s.touch_near_pcm;\n"
"  if(!initial){el('mode').value=String(s.mode);el('prime').value=String(s.prime);el('ui').value=String(s.ui);el('primeValue').textContent=s.prime+' KB';initial=true;}\n"
" }catch(e){el('other').textContent='Diagnose nicht erreichbar: '+e.message}\n"
"}\n"
"function draw(id,arr,selector,range){\n"
" const c=el(id),ctx=c.getContext('2d'),scale=window.devicePixelRatio||1;\n"
" const w=c.clientWidth,h=170;c.width=w*scale;c.height=h*scale;ctx.scale(scale,scale);\n"
" ctx.clearRect(0,0,w,h);\n"
" ctx.strokeStyle='#32485a';ctx.lineWidth=1;\n"
" for(let i=1;i<4;i++){ctx.beginPath();ctx.moveTo(0,h*i/4);ctx.lineTo(w,h*i/4);ctx.stroke();}\n"
" if(arr.length<2)return;\n"
" const ys=arr.map(selector),min=range?0:Math.max(0,Math.min(...ys)*0.9),max=range?range:Math.max(...ys)*1.05+1;\n"
" ctx.beginPath();ctx.strokeStyle='#2dd4bf';ctx.lineWidth=2;\n"
" for(let i=0;i<ys.length;i++){\n"
"  const x=i*w/Math.max(1,ys.length-1),y=h-5-(ys[i]-min)/(max-min)*(h-10);\n"
"  if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);\n"
" }\n"
" ctx.stroke();\n"
" ctx.fillStyle='#a5b7c3';ctx.font='12px system-ui';ctx.fillText((max/1024).toFixed(1)+' KB',5,13);ctx.fillText((min/1024).toFixed(1)+' KB',5,h-5);\n"
"}\n"
"async function history(){\n"
" try{\n"
"  const r=await fetch('/lab/history',{cache:'no-store'});if(!r.ok)return;\n"
"  const items=await r.json();\n"
"  draw('pcmChart',items,x=>x.pcm,65536);\n"
"  draw('ramChart',items,x=>x.ram,0);\n"
" }catch(e){}\n"
"}\n"
"refresh();history();setInterval(refresh,1000);setInterval(history,3000);\n"
"</script></body></html>\n";

static esp_err_t lab_home(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, LAB_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t lab_state(httpd_req_t *req)
{
    radio_diag_snapshot_t d;
    radio_diag_snapshot(&d);
    wifi_ap_record_t ap = {0};
    bool have_rssi = radio_wifi_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    char response[1152];
    int n = snprintf(response, sizeof(response),
        "{\"mode\":%d,\"prime\":%lu,\"ui\":%lu,\"pcm_bytes\":%lu,"
        "\"pcm_capacity\":%lu,\"pcm_min\":%lu,\"pcm_max\":%lu,"
        "\"pcm_underruns\":%lu,\"touch_near_pcm\":%lu,"
        "\"i2s_errors\":%lu,\"i2s_calls\":%lu,\"i2s_stack\":%lu,"
        "\"codec_max\":%lu,\"decoder_max\":%lu,"
        "\"ram_free\":%lu,\"ram_largest\":%lu,"
        "\"fifo_bytes\":%lu,\"fifo_capacity\":%lu,"
        "\"wifi_lost\":%lu,\"touches\":%lu,\"marks\":%lu,"
        "\"http_bytes\":%lu,\"rssi\":",
        radio_lab_mode(), (unsigned long)radio_lab_prime_kb(),
        (unsigned long)radio_ui_interval_get(),
        (unsigned long)d.pcm_bytes, (unsigned long)d.pcm_capacity,
        (unsigned long)d.pcm_min_bytes, (unsigned long)d.pcm_max_bytes,
        (unsigned long)d.pcm_underruns, (unsigned long)d.pcm_near_touch,
        (unsigned long)d.i2s_write_errors, (unsigned long)d.i2s_write_calls,
        (unsigned long)d.i2s_stack_min_bytes,
        (unsigned long)d.codec_max_ms, (unsigned long)d.decoder_max_ms,
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long)d.fifo_bytes, (unsigned long)d.fifo_capacity,
        (unsigned long)d.wifi_disconnects, (unsigned long)d.touch_presses,
        (unsigned long)d.marks, (unsigned long)d.http_bytes);
    if (n < 0 || n >= (int)sizeof(response) - 24) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    if (have_rssi) snprintf(response + n, sizeof(response) - n,
                           "%d}", (int)ap.rssi);
    else snprintf(response + n, sizeof(response) - n, "null}");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t lab_history(httpd_req_t *req)
{
    /* Return the time series in ONE response rather than 120 small writes.
     * Otherwise observing the lab would perturb the Wi-Fi/audio timing. */
    radio_diag_sample_t *items = heap_caps_malloc(
        sizeof(radio_diag_sample_t) * RADIO_DIAG_SAMPLES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *response = heap_caps_malloc(16000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!items || !response) {
        heap_caps_free(items);
        heap_caps_free(response);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t count = radio_diag_history(items, RADIO_DIAG_SAMPLES);
    const size_t capacity = 16000;
    size_t used = 0;
    response[used++] = '[';
    for (size_t i = 0; i < count; ++i) {
        int n = snprintf(response + used, capacity - used,
             "%s{\"t\":%lu,\"pcm\":%lu,\"ram\":%lu,\"http\":%lu,\"under\":%lu,\"touch\":%lu}",
             i ? "," : "", (unsigned long)items[i].ms,
             (unsigned long)items[i].pcm_bytes,
             (unsigned long)items[i].internal_free,
             (unsigned long)items[i].http_bytes,
             (unsigned long)items[i].pcm_underruns,
             (unsigned long)items[i].touch_presses);
        if (n < 0 || (size_t)n >= capacity - used - 2) {
            heap_caps_free(items);
            heap_caps_free(response);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        used += (size_t)n;
    }
    response[used++] = ']';
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, response, used);
    heap_caps_free(items);
    heap_caps_free(response);
    return err;
}

static bool parse_num(const char *data, const char *key, long *out)
{
    char buf[20] = {0};
    if (!url_form_get(data, key, buf, sizeof(buf))) return false;
    if (!buf[0]) return false;
    char *end = NULL;
    long n = strtol(buf, &end, 10);
    if (!end || *end) return false;
    *out = n;
    return true;
}

static esp_err_t lab_set(httpd_req_t *req)
{
    if (req->content_len < 0 || req->content_len > 180) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Ungueltige Laenge");
        return ESP_FAIL;
    }
    char body[192] = {0};
    size_t done = 0;
    while (done < (size_t)req->content_len) {
        int n = httpd_req_recv(req, body + done,
                               (size_t)req->content_len - done);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
        done += n;
    }
    long mode = 0, prime = 0, ui = 0;
    if (!parse_num(body, "mode", &mode) || mode < 0 || mode > 2 ||
        !parse_num(body, "prime", &prime) || prime < 8 || prime > 48 ||
        prime % 4 != 0 ||
        !parse_num(body, "ui", &ui) ||
        (ui != 500 && ui != 1000 && ui != 2000)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Parameter nicht erlaubt");
        return ESP_FAIL;
    }
    (void)radio_lab_set_prime_kb((uint32_t)prime);
    (void)radio_ui_interval_set((uint32_t)ui);
    (void)radio_lab_set_mode((int)mode);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "Test-Einstellungen aktiv (nicht gespeichert).");
}

static esp_err_t lab_mark(httpd_req_t *req)
{
    radio_diag_mark();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "Stoerung mit Zeitstempel markiert.");
}

static esp_err_t lab_csv(httpd_req_t *req)
{
    radio_diag_sample_t *items = heap_caps_malloc(
        sizeof(radio_diag_sample_t) * RADIO_DIAG_SAMPLES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!items) { httpd_resp_send_500(req); return ESP_FAIL; }
    size_t count = radio_diag_history(items, RADIO_DIAG_SAMPLES);
    radio_diag_snapshot_t d;
    radio_diag_snapshot(&d);

    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=kuechenradio65_diagnose.csv");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req,
        "typ;zeit_ms;pcm_bytes;http_bytes;pcm_unterlaeufe;touch_anzahl;intern_frei_bytes;"
        "ereignis;wert;zusatz;http_alter_ms;audio_alter_ms;lvgl_alter_ms\n",
        HTTPD_RESP_USE_STRLEN);
    char row[256];
    for (size_t i = 0; i < count && err == ESP_OK; ++i) {
        int n = snprintf(row, sizeof(row),
            "sample;%lu;%lu;%lu;%lu;%lu;%lu;;;;;;\n",
            (unsigned long)items[i].ms,
            (unsigned long)items[i].pcm_bytes,
            (unsigned long)items[i].http_bytes,
            (unsigned long)items[i].pcm_underruns,
            (unsigned long)items[i].touch_presses,
            (unsigned long)items[i].internal_free);
        if (n < 0 || n >= (int)sizeof(row)) { err = ESP_FAIL; break; }
        err = httpd_resp_send_chunk(req, row, n);
    }
    for (size_t i = 0; i < d.event_count && err == ESP_OK; ++i) {
        const radio_diag_event_t *e = &d.events[i];
        int n = snprintf(row, sizeof(row),
            "event;%lu;%lu;;;;;%s;%lu;%d;%ld;%ld;%ld\n",
            (unsigned long)e->ms, (unsigned long)e->pcm_bytes,
            radio_diag_name((radio_diag_kind_t)e->kind),
            (unsigned long)e->value, (int)e->aux,
            e->network_age_ms == UINT32_MAX ? -1L : (long)e->network_age_ms,
            e->audio_age_ms == UINT32_MAX ? -1L : (long)e->audio_age_ms,
            e->ui_age_ms == UINT32_MAX ? -1L : (long)e->ui_age_ms);
        if (n < 0 || n >= (int)sizeof(row)) { err = ESP_FAIL; break; }
        err = httpd_resp_send_chunk(req, row, n);
    }
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    heap_caps_free(items);
    return err;
}

esp_err_t radio_lab_web_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri="/lab", .method=HTTP_GET, .handler=lab_home},
        {.uri="/lab/state", .method=HTTP_GET, .handler=lab_state},
        {.uri="/lab/history", .method=HTTP_GET, .handler=lab_history},
        {.uri="/lab/set", .method=HTTP_POST, .handler=lab_set},
        {.uri="/lab/mark", .method=HTTP_POST, .handler=lab_mark},
        {.uri="/lab/csv", .method=HTTP_GET, .handler=lab_csv},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        esp_err_t e = httpd_register_uri_handler(server, &routes[i]);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}
