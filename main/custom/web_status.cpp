// ESP32 Time Server - web status server
// License: MIT
// Website: https://github.com/SinclairKS/ESP32TimeServer
//
// Read-only web status server.
//
// This is a *small* auxiliary server whose only job is to expose the
// system status information (time, sync/PPS state, NTP and GNSS counters,
// memory) as a human readable page and as JSON, for monitoring purposes.
//
// Design constraints (important for a Stratum 1 time server):
//
// 1. It is read-only. It never touches the PPS discipline path, the GNSS
//    processing, or the NTP reply path - it only reads atomics / snapshots
//    that are already maintained for MQTT reporting.
//
// 2. It runs on FreeRTOS core 0 at a low priority (below every time
//    sensitive task) so that occasional HTML/JSON rendering can never
//    interfere with the timing critical work, which is pinned to core 1
//    (hardware NTP server, PPS discipline) and the high priority NTP
//    server task.
//
// 3. It is intentionally small: a handful of counters, one static HTML
//    template, one fixed size response buffer, and a couple of concurrent
//    connections. It adds only a few tens of KB to the firmware image.

#include "ESP32TimeServerSettings.h"

#if WEB_STATUS_SERVER_ENABLED

#include "web_status.h"

#include <cstdio>
#include <cstdint>

extern "C"
{
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
}

static const char *TAG = "web_status";

static constexpr size_t WEB_STATUS_RESPONSE_SIZE = 2048;
static constexpr UBaseType_t WEB_STATUS_TASK_PRIORITY = 5; // below every time sensitive task
static constexpr uint32_t WEB_STATUS_TASK_STACK_SIZE = 4096;
static constexpr uint8_t WEB_STATUS_CORE_ID = 0; // keep core 1 free for the timing critical path

static httpd_handle_t s_web_server = nullptr;

// A single buffer, reused for every /status response. This is safe:
// esp_http_server (v6.1) runs ONE server task that services all open
// connections serially through a single select() loop, so two request
// handlers are never in flight at the same time (the other open sockets
// simply wait in the fd queue), and httpd_resp_send() finishes transmitting
// the whole buffer before it returns. Keeping one static buffer also avoids
// a per-request heap allocation. (Do NOT "fix" this to per-connection
// allocation/thread_local on the assumption of concurrent handlers - the
// single-task, serial-dispatch model above is what makes it safe.)
static char s_web_status_response[WEB_STATUS_RESPONSE_SIZE] = "";

// Filled in by main.cpp from its private status state (atomics / snapshots)

void web_status_fill_snapshot(web_status_snapshot_t *s);

static esp_err_t web_status_json_handler(httpd_req_t *req)
{
    web_status_snapshot_t s{};
    web_status_fill_snapshot(&s);

    char *buf = s_web_status_response;
    size_t size = WEB_STATUS_RESPONSE_SIZE;
    size_t len = 0;

    len += static_cast<size_t>(snprintf(buf + len, size - len,
                                        "{\"app\":\"%s\","
                                        "\"version\":\"%s\","
                                        "\"git_sha\":\"%s\","
                                        "\"build_date\":\"%s %s\","
                                        "\"epoch\":%lld,"
                                        "\"uptime_seconds\":%llu,"
                                        "\"stratum\":%u,"
                                        "\"stratum_one\":%s,"
                                        "\"reference_id\":\"%s\","
                                        "\"pps_active\":%s,"
                                        "\"gnss_timing_valid\":%s,"
                                        "\"holdover\":%s,"
                                        "\"pps_missing\":%s,"
                                        "\"gnss_invalid\":%s,"
                                        "\"sanity_mismatch\":%s,"
                                        "\"sync_stale\":%s,"
                                        "\"last_successful_sync_us\":%lld,"
                                        "\"last_sync_delta\":%lld,"
                                        "\"satellites\":%u,"
                                        "\"ntp_requests_total\":%llu,"
                                        "\"ntp_responses_total\":%u,"
                                        "\"ntp_responses_synced\":%u,"
                                        "\"ntp_responses_gnss_unsynced\":%u,"
                                        "\"ntp_responses_pps_undisciplined\":%u,"
                                        "\"ntp_peak_requests_per_second\":%u,"
                                        "\"load_shedding_recent\":%s,"
                                        "\"free_heap_bytes\":%u,"
                                        "\"min_free_heap_bytes\":%u,"
                                        "\"psram_free_bytes\":%u,"
                                        "\"has_psram\":%s,"
                                        "\"ethernet_connected\":%s,"
                                        "\"ip\":\"%s\"}",
                                        APP_PROJECT_NAME,
                                        APP_VERSION,
                                        APP_GIT_SHA,
                                        APP_BUILD_DATE,
                                        APP_BUILD_TIME,
                                        static_cast<long long>(s.wall_clock),
                                        static_cast<unsigned long long>(s.uptime_us / 1000000ULL),
                                        static_cast<unsigned int>(s.stratum),
                                        s.stratum_one ? "true" : "false",
                                        s.reference_id,
                                        s.pps_active ? "true" : "false",
                                        s.gnss_timing_valid ? "true" : "false",
                                        s.holdover ? "true" : "false",
                                        s.pps_missing ? "true" : "false",
                                        s.gnss_invalid ? "true" : "false",
                                        s.sanity_mismatch ? "true" : "false",
                                        s.sync_stale ? "true" : "false",
                                        static_cast<long long>(s.last_successful_sync_us),
                                        static_cast<long long>(s.last_sync_delta),
                                        static_cast<unsigned int>(s.satellites),
                                        static_cast<unsigned long long>(s.ntp_requests_total),
                                        static_cast<unsigned int>(s.ntp_responses_total),
                                        static_cast<unsigned int>(s.ntp_responses_synced),
                                        static_cast<unsigned int>(s.ntp_responses_gnss_unsynced),
                                        static_cast<unsigned int>(s.ntp_responses_pps_undisciplined),
                                        static_cast<unsigned int>(s.ntp_peak_requests_per_second),
                                        s.load_shedding_recent ? "true" : "false",
                                        static_cast<unsigned int>(s.free_heap_bytes),
                                        static_cast<unsigned int>(s.min_free_heap_bytes),
                                        static_cast<unsigned int>(s.psram_free_bytes),
                                        s.has_psram ? "true" : "false",
                                        s.ethernet_connected ? "true" : "false",
                                        s.ip));

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, static_cast<int>(len));
}

// Single page "command deck" dashboard. It fetches /status every two
// seconds and renders a hero clock (with the full date), an uptime figure,
// a row of health "pills", and three dense Timing / NTP / System columns.
// The whole page is centered and capped at ~65% of the viewport width so it
// does not stretch edge to edge. Deliberately plain (no external assets) so
// that it works with no internet access and a small flash footprint.

static constexpr char WEB_STATUS_HTML[] =
    "<!DOCTYPE html>\n"
    "<html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>ESP32 Time Server</title>"
    "<style>"
    "body{font-family:system-ui,monospace;background:#101418;color:#dfe7ee;margin:0}"
    ".wrap{width:65%;margin:0 auto;padding:24px}"
    "@media(max-width:900px){.wrap{width:100%}}"
    ".hero{display:flex;justify-content:space-between;align-items:flex-end;padding-bottom:14px;border-bottom:1px solid #2a3340}"
    ".time{font-size:58px;line-height:1;font-weight:650;font-variant-numeric:tabular-nums;letter-spacing:-.02em}"
    ".date{color:#8b98a5;font-size:13px;margin-top:6px}"
    ".up{text-align:right}"
    ".up .k{color:#8b98a5;font-size:11px;text-transform:uppercase;letter-spacing:.04em}"
    ".up .v{font-size:16px}"
    ".pills{display:flex;flex-wrap:wrap;gap:8px;padding:12px 0;border-bottom:1px solid #2a3340}"
    ".pill{display:inline-flex;align-items:center;gap:6px;border:1px solid #2a3340;border-radius:999px;padding:3px 12px;font-size:12px;font-variant-numeric:tabular-nums}"
    ".pill.ok{color:#39d98a;border-color:#39d98a66}"
    ".pill.bad{color:#f0616d;border-color:#f0616d66}"
    ".cols{display:grid;grid-template-columns:1fr 1fr 1fr;gap:24px;padding-top:14px}"
    "@media(max-width:900px){.cols{grid-template-columns:1fr}}"
    ".sec{font-size:11px;text-transform:uppercase;letter-spacing:.08em;color:#8b98a5;margin-bottom:6px;font-weight:600}"
    ".row{display:flex;justify-content:space-between;gap:12px;padding:5px 0;border-bottom:1px solid #2a334088}"
    ".row:last-child{border-bottom:none}"
    ".k{color:#8b98a5;font-size:11px;text-transform:uppercase;letter-spacing:.04em}"
    ".v{font-size:14px;font-variant-numeric:tabular-nums;text-align:right}"
    ".ok{color:#39d98a}.warn{color:#f5c542}.bad{color:#f0616d}.off{color:#8b98a5}"
    "footer{margin-top:16px;color:#8b98a5;font-size:.8em}"
    "</style></head><body>"
    "<div class=\"wrap\">"
    "<div class=\"hero\">"
    "<div><div class=\"time\" id=\"clock\">--:--:--</div><div class=\"date\" id=\"date\"></div></div>"
    "<div class=\"up\"><div class=\"k\">uptime</div><div class=\"v\" id=\"uptime\">-</div></div>"
    "</div>"
    "<div class=\"pills\" id=\"pills\"></div>"
    "<div class=\"cols\">"
    "<div><div class=\"sec\">Timing</div><div id=\"col_timing\"></div></div>"
    "<div><div class=\"sec\">NTP</div><div id=\"col_ntp\"></div></div>"
    "<div><div class=\"sec\">System</div><div id=\"col_sys\"></div></div>"
    "</div>"
    "<footer id=\"foot\">loading...</footer>"
    "</div>"
    "<script>"
    "function dur(us){if(us<=0)return'-';var s=Math.floor(us/1e6);var d=Math.floor(s/86400);s%=86400;"
    "var h=Math.floor(s/3600);s%=3600;var m=Math.floor(s/60);var r=Math.floor(s%60);"
    "return(d?d+'d ':'')+(h?h+'h ':'')+m+'m '+r+'s';}"
    "function yn(b,good){return '<span class=\"'+(b?(good||'ok'):'off')+'\">'+(b?'yes':'no')+'</span>'}"
    "function row(k,v){return '<div class=\"row\"><span class=\"k\">'+k+'</span><span class=\"v\">'+v+'</span></div>'}"
    "function pill(t,c){return '<span class=\"pill '+(c||'')+'\">'+t+'</span>'}"
    "function ago(sec){return dur(sec*1e6)+' ago';}"
    "async function refresh(){"
    "try{"
    "var r=await fetch('/status');var s=await r.json();"
    "var now=new Date(s.epoch*1000);"
    "document.getElementById('clock').textContent=now.toLocaleTimeString([],{hour12:false});"
    "document.getElementById('date').textContent=now.toLocaleDateString([],{weekday:'long',year:'numeric',month:'long',day:'numeric'});"
    "document.getElementById('uptime').textContent=dur(s.uptime_seconds*1e6);"
    "var faults=[s.pps_missing&&'PPS',s.gnss_invalid&&'GNSS',s.sanity_mismatch&&'SANITY',s.sync_stale&&'STALE'].filter(Boolean);"
    "var since=s.last_successful_sync_us>0?Math.floor(s.uptime_seconds-s.last_successful_sync_us/1e6):0;"
    "document.getElementById('pills').innerHTML="
    "(s.stratum_one?pill('Stratum 1','ok'):pill('Stratum '+(s.stratum==16?'init':s.stratum),'warn'))+"
    "pill(s.reference_id)+"
    "(s.pps_active?pill('PPS ok','ok'):pill('PPS off'))+"
    "(s.satellites>0?pill(s.satellites+' sats','ok'):pill('0 sats'))+"
    "(s.ethernet_connected?pill('eth up','ok'):pill('eth down','bad'))+"
    "pill(s.ip||'no ip');"
    "document.getElementById('col_timing').innerHTML="
    "row('stratum','<span class=\"'+(s.stratum_one?'ok':'warn')+'\">'+(s.stratum_one?'1 (disciplined)':(s.stratum==16?'16 (init)':s.stratum))+'</span>')+"
    "row('reference','<span class=\"'+(s.reference_id==='GPS'?'ok':'warn')+'\">'+s.reference_id+'</span>')+"
    "row('PPS discipline',yn(s.pps_active))+"
    "row('GNSS timing',yn(s.gnss_timing_valid))+"
    "row('holdover',yn(s.holdover,'warn'))+"
    "row('last sync',s.last_successful_sync_us>0?ago(since):'-')+"
    "row('sync delta',(s.last_sync_delta>0?'+':'')+s.last_sync_delta+' s')+"
    "row('active faults','<span class=\"'+(faults.length?'bad':'ok')+'\">'+(faults.length?faults.join(', '):'none')+'</span>');"
    "document.getElementById('col_ntp').innerHTML="
    "row('requests (total)',s.ntp_requests_total.toLocaleString())+"
    "row('responses (total)',s.ntp_responses_total.toLocaleString())+"
    "row('- synced','<span class=\"ok\">'+s.ntp_responses_synced.toLocaleString()+'</span>')+"
    "row('- GNSS unsynced',s.ntp_responses_gnss_unsynced.toLocaleString())+"
    "row('- PPS undisciplined',s.ntp_responses_pps_undisciplined.toLocaleString())+"
    "row('peak requests/s',s.ntp_peak_requests_per_second.toLocaleString())+"
    "row('load shedding',yn(s.load_shedding_recent,'warn'));"
    "document.getElementById('col_sys').innerHTML="
    "row('ethernet',yn(s.ethernet_connected))+"
    "row('IP address',s.ip||'not yet assigned')+"
    "row('free heap',s.free_heap_bytes.toLocaleString()+' B')+"
    "row('min free heap',s.min_free_heap_bytes.toLocaleString()+' B')+"
    "row('PSRAM free',s.has_psram?(s.psram_free_bytes/1048576).toFixed(1)+' MB':'n/a')+"
    "row('satellites','<span class=\"'+(s.satellites>0?'ok':'off')+'\">'+s.satellites+'</span>')+"
    "row('version',s.version+' ('+s.git_sha+')')+"
    "row('built',s.build_date);"
    "document.getElementById('foot').textContent='updated '+new Date().toLocaleTimeString()+' - refreshes every 2s';"
    "}catch(e){document.getElementById('foot').textContent='status unavailable: '+e}"
    "}"
    "refresh();setInterval(refresh,2000);"
    "</script></body></html>";

static esp_err_t web_status_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, WEB_STATUS_HTML, HTTPD_RESP_USE_STRLEN);
}

void web_server_init()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WebStatusPort;
    // The default ctrl_port (32768) is already taken by the OTE httpd
    // server's loopback control socket; each httpd instance needs a unique
    // one or httpd_start fails with EADDRINUSE.
    config.ctrl_port = 32769;
    config.task_priority = WEB_STATUS_TASK_PRIORITY;
    config.stack_size = WEB_STATUS_TASK_STACK_SIZE;
    config.core_id = WEB_STATUS_CORE_ID;
    config.max_uri_handlers = 4;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5; // seconds

    esp_err_t err = httpd_start(&s_web_server, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Unable to start web status server (port %u): %s", WebStatusPort, esp_err_to_name(err));
        return;
    }

    httpd_uri_t index_uri{};
    index_uri.uri = "/";
    index_uri.method = HTTP_GET;
    index_uri.handler = web_status_index_handler;
    httpd_register_uri_handler(s_web_server, &index_uri);

    httpd_uri_t status_uri{};
    status_uri.uri = "/status";
    status_uri.method = HTTP_GET;
    status_uri.handler = web_status_json_handler;
    httpd_register_uri_handler(s_web_server, &status_uri);

    ESP_LOGI(TAG, "Web status server listening on port %u (core %d, priority %u)",
             WebStatusPort,
             static_cast<int>(WEB_STATUS_CORE_ID),
             static_cast<unsigned int>(WEB_STATUS_TASK_PRIORITY));
}

#endif
