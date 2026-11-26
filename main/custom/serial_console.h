// ESP32 Time Server - web status server: serial console tail
// License: MIT
// Website: https://github.com/SinclairKS/ESP32TimeServer
//
// Captures the application serial console (ESP_LOG output) in a small
// lock-free ring buffer so a live tail of it can be shown on the web
// status page for debugging.
//
// How it works: ESP-IDF routes every ESP_LOG line through a single vprintf
// callback. init() installs a tee that first forwards each formatted line
// unchanged to the C library vprintf (ESP-IDF's default console sink, which
// writes to the console UART) and then appends it to the ring buffer. The
// buffer is drained by the web status httpd task.
//
// What it captures: everything logged after init() returns - in practice the
// whole application console. ROM/bootloader output (before app_main) is not
// visible, and lines compiled out (e.g. DEBUG_ENABLED 0) never appear.
//
// Threading: one writer - ESP-IDF holds the log lock around the vprintf
// call, so lines from all tasks arrive serialized; one reader - the single
// esp_http_server task services all request handlers serially. No locks are
// needed; the only shared state is the two monotonic indices (atomics).
//
// Cost: one 16 KB static buffer in internal RAM, one vsnprintf per log line
// on the log path, and one memcpy into the ring per line.

#pragma once

#include "ESP32TimeServerSettings.h"

#if WEB_STATUS_SERVER_ENABLED && SERIAL_CONSOLE_ENABLED

#include <cstddef>
#include <cstdint>

namespace serial_console
{

// Ring buffer capacity in bytes (power of two).
constexpr size_t kBufferBytes = 16384;

// Install the vprintf tee. Call once, as early as possible - the first
// statement of app_main - so as little of the console as possible is lost.
void init();

// A slice of the ring buffer for the web status page.
struct drain_result_t
{
    uint64_t new_seq = 0; // sequence number one past the last byte returned;
                          // pass it back as the next since_seq
    size_t bytes = 0;     // bytes copied into buf
    uint64_t dropped = 0; // bytes lost to buffer wrap between since_seq and
                          // the data actually returned (0 = no gap)
};

// Return up to `cap` bytes of console text logged at or after `since_seq`.
// Since 0 means "give me the tail". If some of the requested data was
// already overwritten, the oldest still-available bytes are returned and
// `dropped` reports the gap. The buffer is NUL-terminated when non-empty.
drain_result_t drain(uint64_t since_seq, char *buf, size_t cap);

// Sequence number one past the last byte written (0 = empty).
uint64_t seq();

// Total bytes lost to buffer wrap, cumulative since boot.
uint64_t dropped_bytes();

} // namespace serial_console

#endif
