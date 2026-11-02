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
"<title>Küchenradio | Diagnose Plus</title>\n"
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
"<h1>🔬 Küchenradio Diagnoselabor #66</h1>\n"
"<p>Live: letzte 120 Sekunden. CSV: bis zu <b>30 Minuten</b> Zeitverlauf und 320 Ereignisse. Die bisher stabilen Audioeinstellungen bleiben unverändert.</p>\n"
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
"<div class=\"card\"><small>WLAN-Kanal</small><span class=\"kpi\" id=\"wifiChannel\">–</span></div>\n"
"<div class=\"card\"><small>Letzter WLAN-Abbruchgrund</small><span class=\"kpi\" id=\"wifiReason\">–</span></div>\n"
"<div class=\"card\"><small>Letzter HTTP-Read</small><span class=\"kpi\" id=\"httpRead\">–</span></div>\n"
"<div class=\"card\"><small>Letzte HTTP-Daten</small><span class=\"kpi\" id=\"httpAge\">–</span></div>\n"
"<div class=\"card\"><small>HTTP-Reads ≥1 Sek.</small><span class=\"kpi\" id=\"httpSlow\">–</span></div>\n"
"<div class=\"card\"><small>HTTP-Reads ≥4 Sek.</small><span class=\"kpi\" id=\"httpVerySlow\">–</span></div>\n"
"<div class=\"card\"><small>HTTP-Verbindungsaufbau max.</small><span class=\"kpi\" id=\"httpConnect\">–</span></div>\n"
"<div class=\"card\"><small>HTTP-Status</small><span class=\"kpi\" id=\"httpStatus\">–</span></div>\n"
"<div class=\"card\"><small>HTTP-FIFO-Füllstand</small><span class=\"kpi\" id=\"fifoLevel\">–</span></div>\n"
"<div class=\"card\"><small>HTTP-Puffer voll / Wartezeit</small><span class=\"kpi\" id=\"httpFull\">–</span></div>\n"
"<div class=\"card\"><small>Audio-Task-Stackreserve</small><span class=\"kpi\" id=\"audioStack\">–</span></div>\n"
"<div class=\"card\"><small>HTTP-Task-Stackreserve</small><span class=\"kpi\" id=\"httpStack\">–</span></div>\n"
"<div class=\"card\"><small>PSRAM frei</small><span class=\"kpi\" id=\"psram\">–</span></div>\n"
"<div class=\"card\"><small>PCM knapp gefüllt</small><span class=\"kpi\" id=\"pcmLow\">–</span></div>\n"
"<div class=\"card\"><small>CPU- / PSRAM-Takt</small><span class=\"kpi\" id=\"clocks\">–</span></div>\n"
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
"<h2>HTTP-FIFO-Füllstand · letzte 120 Sekunden</h2><canvas id=\"fifoChart\"></canvas>\n"
"<h2>Alter letzter HTTP-Daten · letzte 120 Sekunden</h2><canvas id=\"httpAgeChart\"></canvas>\n"
"<p class=\"caption\">Bei einem HTTP-Read mit 4,5 Sekunden Dauer steigen die Wartezeit und das Alter der letzten Daten – das ist zunächst keine Aussage darüber, ob der Radioserver oder das WLAN verantwortlich war.</p>\n"
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
"<p class=\"caption\">Die Software erfasst Aufrufzeiten, WLAN, HTTP, PCM-Puffer und Task-Reserven. Direkte DAC-Stille, RGB-DMA-Unterläufe und reale Versorgungsspannung sind hier nicht hardwareseitig messbar. RSSI ist ein Empfangssignal-Indikator, kein Paketverlustzähler.</p></section>\n"
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
"  el('rssi').textContent=s.wifi_connected?s.rssi+' dBm':'nicht verbunden';\n"
"  el('wifiChannel').textContent=s.wifi_channel||'–';\n"
"  el('wifiReason').textContent=s.wifi_last_reason||'–';\n"
"  el('httpRead').textContent=s.http_inflight?(s.http_running+' ms (läuft)'):(s.http_last_read+' ms');\n"
"  el('httpAge').textContent=s.http_age<0?'noch keine Daten':s.http_age+' ms';\n"
"  el('httpSlow').textContent=s.http_gt_1000;\n"
"  el('httpVerySlow').textContent=s.http_gt_4000;\n"
"  el('httpConnect').textContent=s.http_connect_max+' ms';\n"
"  el('httpStatus').textContent=s.http_status||'–';\n"
"  el('fifoLevel').textContent=kb(s.fifo_bytes)+'/'+kb(s.fifo_capacity);\n"
"  el('httpFull').textContent=(s.http_wait_full/1000).toFixed(1)+' s';\n"
"  el('audioStack').textContent=s.audio_stack?kb(s.audio_stack):'–';\n"
"  el('httpStack').textContent=s.http_stack?kb(s.http_stack):'–';\n"
"  el('psram').textContent=kb(s.psram_free);\n"
"  el('pcmLow').textContent=s.pcm_low;\n"
"  el('clocks').textContent=s.cpu_mhz+'/'+s.psram_mhz+' MHz';\n"
"  el('other').textContent='HTTP: '+s.http_reads+' Reads, '+s.http_bytes+' Bytes, '+s.http_zero+' x 0 Bytes, '+s.http_negative+' Fehler/EAGAIN, max '+s.http_max+' ms · HTTP ohne Daten maximal '+s.http_age_max+' ms · Verbindungsversuche '+s.http_connect_attempts+', Fehler '+s.http_connect_failures+', Redirects '+s.http_redirects+' · WLAN verbindet '+s.wifi_connects+' mal, Abbrüche '+s.wifi_lost+' · Decoder max '+s.decoder_max+' ms · Audio-FIFO-Unterläufe '+s.fifo_underruns+' · I2S-Calls '+s.i2s_calls+' · Touch '+s.touches+', Markierungen '+s.marks+' · Sample-Rate '+s.sample_rate+' Hz, Kanäle '+s.channels+' · LWIP-Status '+s.http_last_result;\n"
"  if(!initial){el('mode').value=String(s.mode);el('prime').value=String(s.prime);el('ui').value=String(s.ui);el('primeValue').textContent=s.prime+' KB';initial=true;}\n"
" }catch(e){el('other').textContent='Diagnose nicht erreichbar: '+e.message}\n"
"}\n"
"function draw(id,arr,selector,range,unit='KB'){\n"
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
" ctx.fillStyle='#a5b7c3';ctx.font='12px system-ui';const scaleValue=unit==='ms'?1:1024;ctx.fillText((max/scaleValue).toFixed(1)+' '+unit,5,13);ctx.fillText((min/scaleValue).toFixed(1)+' '+unit,5,h-5);\n"
"}\n"
"async function history(){\n"
" try{\n"
"  const r=await fetch('/lab/history',{cache:'no-store'});if(!r.ok)return;\n"
"  const items=await r.json();\n"
"  draw('pcmChart',items,x=>x.pcm,65536);\n"
"  draw('ramChart',items,x=>x.ram,0);\n"
"  draw('fifoChart',items,x=>x.fifo,32768);\n"
"  draw('httpAgeChart',items,x=>Math.min(x.http_age<0?0:x.http_age,10000),10000,'ms');\n"
" }catch(e){}\n"
"}\n"
"refresh();history();setInterval(refresh,1000);setInterval(history,3000);\n"
"</script></body></html>\n"
"\n";

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
    char response[3000];
    int32_t http_age = d.last_http_ms ? (int32_t)(d.uptime_ms - d.last_http_ms) : -1;
    uint32_t running_ms = d.http_inflight && d.http_read_started_ms ?
                          d.uptime_ms - d.http_read_started_ms : 0;
    int n = snprintf(response, sizeof(response),
        "{\"mode\":%d,\"prime\":%lu,\"ui\":%lu,"
        "\"pcm_bytes\":%lu,\"pcm_capacity\":%lu,\"pcm_min\":%lu,\"pcm_max\":%lu,"
        "\"pcm_underruns\":%lu,\"touch_near_pcm\":%lu,\"pcm_low\":%lu,"
        "\"i2s_errors\":%lu,\"i2s_calls\":%lu,\"i2s_stack\":%lu,"
        "\"codec_max\":%lu,\"decoder_max\":%lu,\"ram_free\":%lu,\"ram_largest\":%lu,"
        "\"psram_free\":%lu,\"fifo_bytes\":%lu,\"fifo_capacity\":%lu,\"fifo_underruns\":%lu,"
        "\"wifi_lost\":%lu,\"wifi_connects\":%lu,\"wifi_last_reason\":%lu,"
        "\"wifi_connected\":%lu,\"wifi_channel\":%lu,\"rssi\":%ld,"
        "\"touches\":%lu,\"marks\":%lu,"
        "\"http_bytes\":%lu,\"http_reads\":%lu,\"http_last_read\":%lu,"
        "\"http_last_result\":%ld,\"http_inflight\":%lu,\"http_running\":%lu,"
        "\"http_max\":%lu,\"http_gt_1000\":%lu,\"http_gt_4000\":%lu,"
        "\"http_zero\":%lu,\"http_negative\":%lu,\"http_age_max\":%lu,"
        "\"http_age\":%ld,\"http_connect_max\":%lu,\"http_connect_attempts\":%lu,"
        "\"http_connect_failures\":%lu,\"http_status\":%lu,\"http_redirects\":%lu,"
        "\"http_wait_full\":%lu,\"audio_stack\":%lu,\"http_stack\":%lu,"
        "\"sample_rate\":%lu,\"channels\":%lu,\"cpu_mhz\":%lu,\"psram_mhz\":%lu}",
        radio_lab_mode(), (unsigned long)radio_lab_prime_kb(),
        (unsigned long)radio_ui_interval_get(),
        (unsigned long)d.pcm_bytes,(unsigned long)d.pcm_capacity,
        (unsigned long)d.pcm_min_bytes,(unsigned long)d.pcm_max_bytes,
        (unsigned long)d.pcm_underruns,(unsigned long)d.pcm_near_touch,
        (unsigned long)d.pcm_low_events,
        (unsigned long)d.i2s_write_errors,(unsigned long)d.i2s_write_calls,
        (unsigned long)d.i2s_stack_min_bytes,
        (unsigned long)d.codec_max_ms,(unsigned long)d.decoder_max_ms,
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        (unsigned long)d.fifo_bytes,(unsigned long)d.fifo_capacity,
        (unsigned long)d.fifo_underruns,
        (unsigned long)d.wifi_disconnects,(unsigned long)d.wifi_connects,
        (unsigned long)d.wifi_last_reason,
        (unsigned long)d.wifi_is_connected,(unsigned long)d.wifi_channel,
        (long)d.wifi_rssi,
        (unsigned long)d.touch_presses,(unsigned long)d.marks,
        (unsigned long)d.http_bytes,(unsigned long)d.http_reads,
        (unsigned long)d.http_last_read_ms,(long)d.http_last_result,
        (unsigned long)d.http_inflight,(unsigned long)running_ms,
        (unsigned long)d.http_max_ms,(unsigned long)d.http_read_gt_1000ms,
        (unsigned long)d.http_read_gt_4000ms,
        (unsigned long)d.http_zero_reads,(unsigned long)d.http_negative_reads,
        (unsigned long)d.http_age_max_ms,(long)http_age,
        (unsigned long)d.http_connect_max_ms,(unsigned long)d.http_connect_attempts,
        (unsigned long)d.http_connect_failures,(unsigned long)d.http_status,
        (unsigned long)d.http_redirects,(unsigned long)d.http_wait_full_ms,
        (unsigned long)d.audio_stack_free,(unsigned long)d.http_stack_free,
        (unsigned long)d.stream_sample_rate,(unsigned long)d.stream_channels,
        (unsigned long)d.cpu_mhz_config,(unsigned long)d.psram_mhz_config);
    if (n < 0 || n >= (int)sizeof(response)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, response, n);
}

static esp_err_t lab_history(httpd_req_t *req)
{
    /* Return the time series in ONE response rather than 120 small writes.
     * Otherwise observing the lab would perturb the Wi-Fi/audio timing. */
    radio_diag_sample_t *items = heap_caps_malloc(
        sizeof(radio_diag_sample_t) * 120,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *response = heap_caps_malloc(20000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!items || !response) {
        heap_caps_free(items);
        heap_caps_free(response);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t count = radio_diag_history(items, 120);
    const size_t capacity = 20000;
    size_t used = 0;
    response[used++] = '[';
    for (size_t i = 0; i < count; ++i) {
        int n = snprintf(response + used, capacity - used,
             "%s{\"t\":%lu,\"pcm\":%lu,\"ram\":%lu,\"http\":%lu,\"fifo\":%lu,\"http_age\":%ld,\"rssi\":%d,\"under\":%lu,\"touch\":%lu}",
             i ? "," : "", (unsigned long)items[i].ms,
             (unsigned long)items[i].pcm_bytes,
             (unsigned long)items[i].internal_free,
             (unsigned long)items[i].http_bytes,
             (unsigned long)items[i].fifo_bytes,
             items[i].http_age_ms == UINT32_MAX ? -1L : (long)items[i].http_age_ms,
             (int)items[i].wifi_rssi,
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
    radio_diag_sample_t *samples = heap_caps_malloc(
        RADIO_DIAG_SAMPLES * sizeof(radio_diag_sample_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    radio_diag_event_t *events = heap_caps_malloc(
        RADIO_DIAG_EXT_EVENTS * sizeof(radio_diag_event_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *chunk = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!samples || !events || !chunk) {
        heap_caps_free(samples);
        heap_caps_free(events);
        heap_caps_free(chunk);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t count = radio_diag_history(samples, RADIO_DIAG_SAMPLES);
    size_t ev_count = radio_diag_extended_events(events, RADIO_DIAG_EXT_EVENTS);
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=kuechenradio66_diagnose.csv");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const char *header =
        "typ;zeit_ms;pcm_bytes;fifo_bytes;http_bytes;http_alter_ms;http_read_ms;"
        "pcm_unterlaeufe;fifo_unterlaeufe;i2s_fehler;touch_anzahl;"
        "intern_frei_bytes;intern_groesster_block;psram_frei_bytes;"
        "wifi_rssi_dbm;wifi_kanal;wifi_verbunden;"
        "audio_stack_frei;http_stack_frei;i2s_stack_frei;modus;"
        "ereignis;wert;zusatz;event_pcm;event_fifo;event_http_alter_ms;"
        "event_audio_alter_ms;event_lvgl_alter_ms\n";
    esp_err_t err = httpd_resp_send_chunk(req, header, HTTPD_RESP_USE_STRLEN);
    size_t used = 0;
    char row[300];
    for (size_t i = 0; i < count && err == ESP_OK; ++i) {
        const radio_diag_sample_t *s = &samples[i];
        int n = snprintf(row, sizeof(row),
            "sample;%lu;%lu;%lu;%lu;%ld;%lu;%lu;%lu;%lu;%lu;%lu;%lu;%lu;"
            "%d;%u;%u;%lu;%lu;%lu;%u;;;;;;;;\n",
            (unsigned long)s->ms,(unsigned long)s->pcm_bytes,
            (unsigned long)s->fifo_bytes,(unsigned long)s->http_bytes,
            s->http_age_ms == UINT32_MAX ? -1L : (long)s->http_age_ms,
            (unsigned long)s->http_last_read_ms,
            (unsigned long)s->pcm_underruns,(unsigned long)s->fifo_underruns,
            (unsigned long)s->i2s_errors,(unsigned long)s->touch_presses,
            (unsigned long)s->internal_free,(unsigned long)s->internal_largest,
            (unsigned long)s->psram_free,(int)s->wifi_rssi,
            (unsigned)s->wifi_channel,(unsigned)s->wifi_connected,
            (unsigned long)s->audio_stack_free,(unsigned long)s->http_stack_free,
            (unsigned long)s->i2s_stack_free,(unsigned)s->mode);
        if (n < 0 || n >= (int)sizeof(row)) { err = ESP_FAIL; break; }
        if (used + n >= 4096) {
            err = httpd_resp_send_chunk(req, chunk, used);
            used = 0;
        }
        if (err != ESP_OK) break;
        memcpy(chunk + used, row, n);
        used += n;
    }
    for (size_t i = 0; i < ev_count && err == ESP_OK; ++i) {
        const radio_diag_event_t *e = &events[i];
        int n = snprintf(row, sizeof(row),
            "event;%lu;;;;;;;;;;;;;;;;;;;;%s;%lu;%d;%lu;%lu;%ld;%ld;%ld\n",
            (unsigned long)e->ms, radio_diag_name((radio_diag_kind_t)e->kind),
            (unsigned long)e->value, (int)e->aux,
            (unsigned long)e->pcm_bytes,(unsigned long)e->fifo_bytes,
            e->network_age_ms == UINT32_MAX ? -1L : (long)e->network_age_ms,
            e->audio_age_ms == UINT32_MAX ? -1L : (long)e->audio_age_ms,
            e->ui_age_ms == UINT32_MAX ? -1L : (long)e->ui_age_ms);
        if (n < 0 || n >= (int)sizeof(row)) { err = ESP_FAIL; break; }
        if (used + n >= 4096) {
            err = httpd_resp_send_chunk(req, chunk, used);
            used = 0;
        }
        if (err != ESP_OK) break;
        memcpy(chunk + used, row, n);
        used += n;
    }
    if (err == ESP_OK && used) err = httpd_resp_send_chunk(req, chunk, used);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    heap_caps_free(samples);
    heap_caps_free(events);
    heap_caps_free(chunk);
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
