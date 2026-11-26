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
// See web_status.cpp for the design constraints (read-only, pinned to
// FreeRTOS core 0, low priority, minimal footprint).

#pragma once

#include "ESP32TimeServerSettings.h"

#if WEB_STATUS_SERVER_ENABLED

#include <cstdint>

// A flat, read-only snapshot of the system status. main.cpp fills this in
// from the atomics / snapshots it already maintains for MQTT reporting, so
// the web server has no direct access to the timing critical state - it
// only ever reads a consistent copy.
struct web_status_snapshot_t
{
    int64_t wall_clock = 0; // current UTC wall clock, seconds since 1970 (epoch)
    uint64_t uptime_us = 0; // system uptime, microseconds

    uint8_t stratum = 16;       // 1 when Stratum 1, 16 when not yet valid
    bool stratum_one = false;   // true when acting as a Stratum 1 source
    char reference_id[5] = "INIT";

    bool pps_active = false;
    bool gnss_timing_valid = false;
    bool holdover = false;
    bool pps_missing = false;
    bool gnss_invalid = false;
    bool sanity_mismatch = false;
    bool sync_stale = false;

    int64_t last_successful_sync_us = 0; // esp_timer_get_time() of last good sync
    int64_t last_sync_delta = 0;         // seconds between last two GNSS fixes

    uint8_t satellites = 0;

    uint64_t ntp_requests_total = 0;
    uint32_t ntp_responses_total = 0;
    uint32_t ntp_responses_synced = 0;
    uint32_t ntp_responses_gnss_unsynced = 0;
    uint32_t ntp_responses_pps_undisciplined = 0;
    uint32_t ntp_peak_requests_per_second = 0;
    bool load_shedding_recent = false;

    uint32_t free_heap_bytes = 0;
    uint32_t min_free_heap_bytes = 0;
    uint32_t psram_free_bytes = 0;
    bool has_psram = false;

    bool ethernet_connected = false;
    char ip[46] = ""; // IPv4 or IPv6 text (IP_ADDRESS_TEXT_SIZE = INET6_ADDRSTRLEN = 46)
};

// Filled in by main.cpp from its private status state (atomics / snapshots)
void web_status_fill_snapshot(web_status_snapshot_t *s);

// Starts the httpd server (call once from app_main, after ethernet is up)
void web_server_init();

#endif
