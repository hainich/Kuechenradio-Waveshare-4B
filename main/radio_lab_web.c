#include "radio_lab_web.h"
#include "radio_stream.h"
#include "radio_ui.h"
#include "radio_diag.h"
#include "radio_wifi.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "url_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Static, self-contained HTML: no external JavaScript, images or CSS assets. */
static const char LAB_PAGE[] =
"<!doctype html>\n"
"<html lang=\"de\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>Küchenradio · Diagnose #70</title>\n"
"<style>\n"
":root{font-family:system-ui,sans-serif;color-scheme:dark;background:#0b1422;color:#e5edf5}\n"
"body{max-width:860px;margin:auto;padding:12px}\n"
"h1{font-size:1.45rem;color:#67e8f9}h2{font-size:1.1rem;margin:8px 0;color:#a5f3fc}\n"
"p,small{color:#b6c5d2}a{color:#67e8f9}section{border:1px solid #294050;border-radius:10px;background:#15263a;padding:14px;margin:11px 0}\n"
"button,select{font:inherit;background:#0f766e;color:white;border:0;border-radius:7px;padding:10px;cursor:pointer}\n"
"button.alt,select{background:#31475e}button.hot{background:#9a3412;font-weight:700}\n"
"button:disabled{opacity:.5}label{display:block;margin:8px 0}input[type=range]{accent-color:#2dd4bf;width:100%}\n"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(144px,1fr));gap:7px;margin:9px 0}\n"
".metric{background:#0c1b2d;border-radius:7px;padding:9px;min-height:58px}\n"
".metric small{display:block;font-size:.75rem}.metric strong{display:block;margin-top:3px;font-size:1.05rem;word-break:break-word}\n"
".row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}.row>*{flex:1}\n"
"select{width:100%}canvas{display:block;width:100%;height:145px;background:#07131f;margin:10px 0}\n"
"#message{min-height:22px;border-left:3px solid #2dd4bf;padding:6px 10px;color:#c8eef0}\n"
"</style></head><body>\n"
"<h1>Küchenradio · Diagnose #70</h1>\n"
"<p><a href=\"/\">Sendereinstellungen</a> · <a href=\"/lab/csv\">30-Minuten-Protokoll (CSV)</a> · <a href=\"/diagnose\">Textbericht</a></p>\n"
"<section>\n"
"<h2>Störung unmittelbar markieren</h2>\n"
"<form id=\"markForm\" method=\"post\" action=\"/lab/mark\">\n"
"<button class=\"hot\" type=\"submit\">Störung gehört / gesehen – Zeit merken</button>\n"
"</form>\n"
"<p id=\"message\">Bereit. Die Live-Abfrage läuft standardmäßig nicht automatisch.</p>\n"
"</section>\n"
"<section>\n"
"<h2>Diagnosewerte</h2>\n"
"<div class=\"row\"><button id=\"refresh\" onclick=\"refresh()\" type=\"button\">Messwerte jetzt laden</button>\n"
"<label><input id=\"auto\" type=\"checkbox\"> Live alle 15 Sekunden</label></div>\n"
"<div id=\"last\" style=\"margin-top:8px;color:#94a3b8\">Keine Messwerte geladen</div>\n"
"<div id=\"metrics\"></div>\n"
"</section>\n"
"<details><summary>Verlaufsgrafiken (nur auf Anforderung)</summary>\n"
"<section>\n"
"<button type=\"button\" onclick=\"loadHistory()\">Letzte 120 Sekunden laden</button>\n"
"<small>Die 30-Minuten-Aufzeichnung läuft unabhängig von dieser Webseite.</small>\n"
"<h2>PCM-Puffer</h2><canvas id=\"pcmChart\"></canvas>\n"
"<h2>HTTP-FIFO</h2><canvas id=\"fifoChart\"></canvas>\n"
"<h2>Interner RAM</h2><canvas id=\"ramChart\"></canvas>\n"
"<h2>HTTP-Datenpause</h2><canvas id=\"httpAgeChart\"></canvas>\n"
"</section></details>\n"
"<details><summary>Temporäre Testeinstellungen</summary><section>\n"
"<p>Die Grundeinstellungen bleiben unverändert. Änderungen sind nur bis zum Neustart gültig.</p>\n"
"<label for=\"mode\">Audio-Testmodus</label>\n"
"<select id=\"mode\">\n"
"<option value=\"0\">Normales Internetradio</option>\n"
"<option value=\"1\">Interner Testton direkt über I2S</option>\n"
"<option value=\"2\">Testton über PCM-Ring</option></select>\n"
"<label for=\"prime\">PCM-Startfüllung: <b id=\"primeValue\">24 KB</b></label>\n"
"<input type=\"range\" id=\"prime\" min=\"8\" max=\"48\" step=\"4\" value=\"24\" oninput=\"q('primeValue').textContent=this.value+' KB'\">\n"
"<label for=\"ui\">Statusintervall am Display</label>\n"
"<select id=\"ui\"><option value=\"500\">500 ms</option>\n"
"<option value=\"1000\" selected>1000 ms</option>\n"
"<option value=\"2000\">2000 ms</option></select>\n"
"<div class=\"row\"><button type=\"button\" onclick=\"apply()\">Einstellungen übernehmen</button>\n"
"<button class=\"alt\" type=\"button\" onclick=\"resetSettings()\">Standardwerte herstellen</button></div>\n"
"</section></details>\n"
"<p><small>Keine automatische Grafik-Abfrage. Keine automatische Änderung der Audio-Taktraten. Ein CSV-Export kann bei sehr knappem RAM langsam sein. ESP32-Diagnose misst keine tatsächliche elektrische DAC-Stille.</small></p>\n"
"<script>\n"
"'use strict';\n"
"const q=id=>document.getElementById(id);\n"
"let busy=false,started=false,autoTimer=null,loading=false,marked=false;\n"
"const groups=[\n"
"['Audio',[['pcm_bytes','PCM aktuell','kb'],['pcm_capacity','PCM-Kapazität','kb'],['pcm_underruns','PCM-Unterläufe'],['fifo_underruns','HTTP-Puffer-Unterläufe'],['pcm_low','PCM knapp gefüllt'],['i2s_errors','I2S-Fehler'],['codec_max','Codec-Write Maximum','ms'],['codec_mutex_max','Codec-Mutex-Wartezeit max.','ms'],['pcm_push_max','PCM-Schreibwartezeit max.','ms'],['pcm_push_count','PCM-Warteereignisse'],['decoder_max','Decoder-Ausführung max.','ms'],['decoder_gap_max','Decoder-Aufrufabstand max.','ms'],['decoder_gap_fifo','Decoder-Pause trotz HTTP-Daten'],['decoder_gap_over_1000','Decoder-Pausen über 1 s']]],\n"
"['Netzwerk',[['rssi','WLAN-RSSI','dbm'],['wifi_channel','WLAN-Kanal'],['wifi_last_reason','Letzter WLAN-Abbruchcode'],['wifi_lost','WLAN-Abbrüche'],['http_last_read','Letzter HTTP-Read','ms'],['http_running','Laufender HTTP-Read','ms'],['http_age','Seit letztem HTTP-Byte','ms'],['http_max','HTTP-Lesezeit max.','ms'],['http_gt_1000','HTTP-Reads über 1 s'],['http_gt_4000','HTTP-Reads über 4 s'],['http_zero','HTTP-Reads ohne Daten'],['http_negative','HTTP-Fehler / EAGAIN'],['http_connect_max','HTTP-Verbindungsaufbau max.','ms'],['http_connect_failures','HTTP-Verbindungsfehler'],['http_status','Letzter HTTP-Status'],['http_bytes','Empfangene Audiodaten','kb'],['fifo_bytes','HTTP-Puffer aktuell','kb'],['fifo_capacity','HTTP-Puffer-Kapazität','kb']]],\n"
"['Speicher / Tasks',[['ram_free','Interner RAM frei','kb'],['ram_largest','Größter interner Block','kb'],['ram_min','Interner RAM-Minimum','kb'],['ram_largest_min','Größter Block-Minimum','kb'],['psram_free','PSRAM frei','kb'],['heap_fail','Allokationsfehler'],['heap_fail_internal','Davon intern angefordert'],['heap_fail_size','Letzte Fehlanforderung','bytes'],['heap_fail_caps','Letzte Heap-Caps','hex'],['heap_fail_function','Allokations-API (Hash)','hex'],['audio_stack','Audio-Stack frei','kb'],['http_stack','HTTP-Stack frei','kb'],['i2s_stack','I2S-Stack frei','kb'],['wifi_stack','WLAN-Task-Stack frei','kb']]],\n"
"['Webserver / Betrieb',[['web_calls','Web-Anfragen gesamt'],['web_errors','Web-Übertragungsfehler'],['web_page_max','Web-Seite max.','ms'],['web_state_max','Web-Status max.','ms'],['web_history_max','Web-Grafik max.','ms'],['web_action_max','Web-Aktion max.','ms'],['web_csv_max','CSV-Export max.','ms'],['cpu_mhz','CPU-Takt','mhz'],['psram_mhz','PSRAM-Takt','mhz'],['touches','Touch-Ereignisse'],['marks','Störungsmarken']]]\n"
"];\n"
"const nodes={};for(const [title,fields] of groups){\n"
"const part=document.createElement('section');const h=document.createElement('h2');h.textContent=title;part.appendChild(h);\n"
"const grid=document.createElement('div');grid.className='grid';\n"
"for(const [key,label] of fields){const d=document.createElement('div');d.className='metric';\n"
"const name=document.createElement('small');name.textContent=label;const val=document.createElement('strong');val.textContent='–';d.append(name,val);grid.appendChild(d);nodes[key]=val;}\n"
"part.appendChild(grid);q('metrics').appendChild(part);\n"
"}\n"
"function fmt(value,unit){\n"
"if(value===undefined||value===null||value<0&&unit==='ms')return '–';\n"
"if(unit==='kb')return (value/1024).toFixed(1)+' KB';\n"
"if(unit==='dbm')return value+' dBm';\n"
"if(unit==='ms')return value+' ms';\n"
"if(unit==='mhz')return value+' MHz';\n"
"if(unit==='bytes')return value+' B';\n"
"if(unit==='hex')return '0x'+Number(value).toString(16);\n"
"return String(value);\n"
"}\n"
"async function send(url,opts={}){\n"
"const ctrl=new AbortController();const t=setTimeout(()=>ctrl.abort(),8000);\n"
"try{const r=await fetch(url,{cache:'no-store',signal:ctrl.signal,...opts});\n"
"if(!r.ok)throw new Error('HTTP '+r.status);return r;}finally{clearTimeout(t);}\n"
"}\n"
"async function refresh(){\n"
"if(loading)return;loading=true;q('refresh').disabled=true;\n"
"try{\n"
"const r=await send('/lab/state');const s=await r.json();\n"
"for(const [title,fields] of groups)for(const [key,label,unit] of fields)\n"
"nodes[key].textContent=fmt(s[key],unit);\n"
"q('last').textContent='Aktualisiert um '+new Date().toLocaleTimeString()+' · letzter HTTP-Read '+(s.http_inflight?'läuft':'abgeschlossen');\n"
"if(!started){started=true;q('mode').value=String(s.mode);q('prime').value=String(s.prime);q('primeValue').textContent=s.prime+' KB';q('ui').value=String(s.ui);}\n"
"}catch(e){q('last').textContent='Status nicht erreichbar: '+e.message;}\n"
"finally{loading=false;q('refresh').disabled=false;}\n"
"}\n"
"const markForm=q('markForm');markForm.addEventListener('submit',async e=>{\n"
"e.preventDefault();q('message').textContent='Markierung wird gesendet…';\n"
"try{const r=await send('/lab/mark',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:''});\n"
"q('message').textContent='✓ '+(await r.text())+' ('+new Date().toLocaleTimeString()+')';}\n"
"catch(err){q('message').textContent='Markierung fehlgeschlagen: '+err.message+'. Bitte erneut klicken.';}\n"
"});\n"
"async function apply(){\n"
"if(busy)return;busy=true;\n"
"try{const body=new URLSearchParams({mode:q('mode').value,prime:q('prime').value,ui:q('ui').value});\n"
"const r=await send('/lab/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});\n"
"q('message').textContent=await r.text();}catch(err){q('message').textContent='Änderung fehlgeschlagen: '+err.message}finally{busy=false;}\n"
"}\n"
"async function resetSettings(){q('mode').value='0';q('prime').value='24';q('ui').value='1000';q('primeValue').textContent='24 KB';await apply();}\n"
"function draw(id,data,key,ceiling,unit){\n"
"const c=q(id);const dpr=window.devicePixelRatio||1;const w=Math.max(240,c.clientWidth);c.width=w*dpr;c.height=145*dpr;\n"
"const ctx=c.getContext('2d');ctx.scale(dpr,dpr);const h=145;ctx.fillStyle='#07131f';ctx.fillRect(0,0,w,h);\n"
"ctx.strokeStyle='#2b4156';for(let k=1;k<4;k++){ctx.beginPath();ctx.moveTo(0,h*k/4);ctx.lineTo(w,h*k/4);ctx.stroke();}\n"
"if(data.length<2)return;\n"
"const ys=data.map(x=>Math.max(0,Number(x[key])||0));const max=ceiling||Math.max(1,...ys)*1.1;\n"
"ctx.strokeStyle='#2dd4bf';ctx.lineWidth=2;ctx.beginPath();ys.forEach((v,i)=>{const x=w*i/(ys.length-1),y=h-8-(Math.min(v,max)/max)*(h-16);i?ctx.lineTo(x,y):ctx.moveTo(x,y);});ctx.stroke();\n"
"ctx.fillStyle='#a5b7c3';ctx.font='12px system-ui';ctx.fillText(unit==='ms'?max+' ms':(max/1024).toFixed(1)+' KB',6,15);\n"
"}\n"
"async function loadHistory(){\n"
"try{q('message').textContent='Verlauf wird geladen…';const r=await send('/lab/history');const a=await r.json();\n"
"draw('pcmChart',a,'pcm',65536,'kb');draw('fifoChart',a,'fifo',32768,'kb');draw('ramChart',a,'ram',0,'kb');draw('httpAgeChart',a,'http_age',10000,'ms');\n"
"q('message').textContent='Verlauf aktualisiert ('+a.length+' Messpunkte).';\n"
"}catch(e){q('message').textContent='Verlauf nicht erreichbar: '+e.message;}\n"
"}\n"
"function autoLoop(){clearTimeout(autoTimer);if(!q('auto').checked)return;\n"
"autoTimer=setTimeout(async()=>{if(!document.hidden)await refresh();autoLoop();},15000);}\n"
"q('auto').addEventListener('change',autoLoop);\n"
"refresh();\n"
"</script></body></html>\n";

static esp_err_t lab_home(httpd_req_t *req)
{
    int64_t start_us = esp_timer_get_time();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_send(req, LAB_PAGE, HTTPD_RESP_USE_STRLEN);
    radio_diag_web_request(1, (uint32_t)((esp_timer_get_time() - start_us) / 1000), result == ESP_OK);
    return result;
}

static esp_err_t lab_state(httpd_req_t *req)
{
    int64_t start_us = esp_timer_get_time();
    radio_diag_snapshot_t d = {0};
    radio_diag_live_snapshot(&d);
    char response[2600];
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
        "\"sample_rate\":%lu,\"channels\":%lu,\"cpu_mhz\":%lu,\"psram_mhz\":%lu,"
        "\"heap_fail\":%lu,\"heap_fail_internal\":%lu,\"heap_fail_size\":%lu,\"heap_fail_caps\":%lu,\"heap_fail_function\":%lu,"
        "\"ram_min\":%lu,\"ram_largest_min\":%lu,\"wifi_stack\":%lu,"
        "\"decoder_gap_max\":%lu,\"decoder_gap_over_1000\":%lu,\"decoder_gap_fifo\":%lu,"
        "\"pcm_push_max\":%lu,\"pcm_push_count\":%lu,\"codec_mutex_max\":%lu,"
        "\"web_calls\":%lu,\"web_errors\":%lu,\"web_page_max\":%lu,\"web_state_max\":%lu,"
        "\"web_history_max\":%lu,\"web_action_max\":%lu,\"web_csv_max\":%lu}",
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
        (unsigned long)d.cpu_mhz_config,(unsigned long)d.psram_mhz_config,
        (unsigned long)d.heap_alloc_fail_count,
        (unsigned long)d.heap_alloc_fail_internal_count,
        (unsigned long)d.heap_alloc_fail_last_bytes,
        (unsigned long)d.heap_alloc_fail_last_caps,
        (unsigned long)d.heap_alloc_fail_function_hash,
        (unsigned long)d.heap_internal_min_bytes,
        (unsigned long)d.heap_internal_largest_min_bytes,
        (unsigned long)d.wifi_stack_free,
        (unsigned long)d.decoder_gap_max_ms,
        (unsigned long)d.decoder_gap_count_1000ms,
        (unsigned long)d.decoder_gap_while_fifo_full,
        (unsigned long)d.pcm_push_block_max_ms,
        (unsigned long)d.pcm_push_block_count,
        (unsigned long)d.codec_mutex_wait_max_ms,
        (unsigned long)d.web_request_count,
        (unsigned long)d.web_error_count,
        (unsigned long)d.web_page_max_ms,
        (unsigned long)d.web_state_max_ms,
        (unsigned long)d.web_history_max_ms,
        (unsigned long)d.web_action_max_ms,
        (unsigned long)d.web_csv_max_ms);
    if (n < 0 || n >= (int)sizeof(response)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_send(req, response, n);
    radio_diag_web_request(2, (uint32_t)((esp_timer_get_time() - start_us) / 1000), result == ESP_OK);
    return result;
}

static esp_err_t lab_history(httpd_req_t *req)
{
    /* Return the time series in ONE response rather than 120 small writes.
     * Otherwise observing the lab would perturb the Wi-Fi/audio timing. */
    radio_diag_sample_t *items = heap_caps_malloc(
        sizeof(radio_diag_sample_t) * 120,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int64_t start_us = esp_timer_get_time();
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
    radio_diag_web_request(3, (uint32_t)((esp_timer_get_time() - start_us) / 1000), err == ESP_OK);
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
    int64_t start_us = esp_timer_get_time();
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
    esp_err_t result = httpd_resp_sendstr(req, "Test-Einstellungen aktiv (nicht gespeichert).");
    radio_diag_web_request(4, (uint32_t)((esp_timer_get_time() - start_us) / 1000), result == ESP_OK);
    return result;
}

static esp_err_t lab_mark(httpd_req_t *req)
{
    int64_t start_us = esp_timer_get_time();
    radio_diag_mark();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t result = httpd_resp_sendstr(req, "Stoerung mit Zeitstempel markiert.");
    radio_diag_web_request(4, (uint32_t)((esp_timer_get_time() - start_us) / 1000), result == ESP_OK);
    return result;
}

static esp_err_t lab_csv(httpd_req_t *req)
{
    int64_t start_us = esp_timer_get_time();
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
                       "attachment; filename=kuechenradio70_diagnose.csv");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const char *header =
        "typ;zeit_ms;pcm_bytes;fifo_bytes;http_bytes;http_alter_ms;http_read_ms;"
        "pcm_unterlaeufe;fifo_unterlaeufe;i2s_fehler;touch_anzahl;"
        "intern_frei_bytes;intern_groesster_block;psram_frei_bytes;"
        "wifi_rssi_dbm;wifi_kanal;wifi_verbunden;"
        "audio_stack_frei;http_stack_frei;i2s_stack_frei;modus;"
        "ereignis;wert;zusatz;event_pcm;event_fifo;event_http_alter_ms;"
        "event_audio_alter_ms;event_lvgl_alter_ms;"
        "heap_fehler_gesamt;decoder_abstand_max_ms;web_antwort_max_ms;"
        "dma_frei_bytes;dma_groesster_block;intern_32bit_frei_bytes\n";
    esp_err_t err = httpd_resp_send_chunk(req, header, HTTPD_RESP_USE_STRLEN);
    size_t used = 0;
    char row[300];
    for (size_t i = 0; i < count && err == ESP_OK; ++i) {
        const radio_diag_sample_t *s = &samples[i];
        int n = snprintf(row, sizeof(row),
            "sample;%lu;%lu;%lu;%lu;%ld;%lu;%lu;%lu;%lu;%lu;%lu;%lu;%lu;"
            "%d;%u;%u;%lu;%lu;%lu;%u;;;;;;;;;%lu;%lu;%lu;%lu;%lu;%lu\n",
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
            (unsigned long)s->i2s_stack_free,(unsigned)s->mode,
            (unsigned long)s->heap_fail_count,
            (unsigned long)s->decoder_gap_max_ms,
            (unsigned long)s->web_max_ms,
            (unsigned long)s->internal_dma_free,
            (unsigned long)s->internal_dma_largest,
            (unsigned long)s->internal_32bit_free);
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
            "event;%lu;;;;;;;;;;;;;;;;;;;;%s;%lu;%d;%lu;%lu;%ld;%ld;%ld;;;\n",
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
    radio_diag_web_request(5, (uint32_t)((esp_timer_get_time() - start_us) / 1000), err == ESP_OK);
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
