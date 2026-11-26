// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_netif.h"

struct native_ote_callbacks_t
{
    void (*progress)(unsigned int, unsigned int);
    void (*failure)(const char *);
    void (*success)();
};

// Call once after Ethernet is initialized. Callbacks run on the HTTP server task.
esp_err_t native_ote_start(esp_netif_t *netif, const native_ote_callbacks_t &callbacks);

// Returns true if the request carries "Authorization: Bearer <OTEPassword>"
// (compared in constant time). Returns false when the OTE password is not set
// (empty string), i.e. OTE authentication is disabled.
bool native_ote_is_authenticated(httpd_req_t *request);
