// ESP32 Time Server - web status server: serial console tail
// License: MIT
// Website: https://github.com/SinclairKS/ESP32TimeServer
//
// See serial_console.h for the design notes.

#include "ESP32TimeServerSettings.h"

#if WEB_STATUS_SERVER_ENABLED && SERIAL_CONSOLE_ENABLED

#include "serial_console.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C"
{
#include "esp_log.h"
}

namespace serial_console
{
namespace
{

// The ring buffer, addressed by monotonic byte index: logical byte n lives
// at slot n & (kBufferBytes - 1). The writer and reader only exchange the
// monotonic indices (atomics below); no per-byte locking is needed.
alignas(64) static char s_buf[kBufferBytes];

// Writer side: the log path. ESP-IDF invokes the vprintf callback with the
// log lock held, so lines from all tasks arrive one at a time.
static std::atomic<uint64_t> s_write_idx{0};

// Reader side: the single httpd task (see web_status.cpp for why the
// handlers are serial).
static std::atomic<uint64_t> s_read_idx{0};
static uint64_t s_total_dropped = 0; // updated only by the reader

// Per-line scratch for the capture pass. Single writer (log lock held),
// so a single static buffer is safe.
constexpr size_t kMaxLineBytes = 1024;
static char s_line[kMaxLineBytes];

// Append one formatted line to the ring. Writer only (log lock held).
void write_bytes(const char *data, size_t len)
{
    const uint64_t base = s_write_idx.load(std::memory_order_relaxed);
    for (size_t i = 0; i < len; ++i)
        s_buf[(base + i) & (kBufferBytes - 1)] = data[i];
    s_write_idx.store(base + len, std::memory_order_release);
}

// The tee: capture the line, then forward it to the real console.
int tee_vprintf(const char *format, va_list args)
{
    // Capture pass (vsnprintf consumes its va_list, so work on a copy).
    va_list capture;
    va_copy(capture, args);
    const int n = vsnprintf(s_line, kMaxLineBytes, format, capture);
    va_end(capture);
    if (n > 0)
    {
        const size_t len =
            (n < static_cast<int>(kMaxLineBytes))
                ? static_cast<size_t>(n)
                : kMaxLineBytes - 1; // over-long line: drop the tail
        write_bytes(s_line, len);
    }

    // Console pass: when no custom vprintf is installed (this project
    // installs none), ESP-IDF's default sink is the C library vprintf,
    // which writes to the console UART. Forwarding there keeps the serial
    // output byte-for-byte unchanged.
    return vprintf(format, args);
}

} // namespace

void init()
{
    esp_log_set_vprintf(tee_vprintf);
}

drain_result_t drain(uint64_t since_seq, char *buf, size_t cap)
{
    drain_result_t r;
    if (cap == 0)
        return r;

    const uint64_t head = s_write_idx.load(std::memory_order_acquire);

    uint64_t start;
    if (since_seq == 0)
    {
        // First poll: the tail.
        start = (head > cap) ? head - cap : 0;
    }
    else
    {
        start = since_seq;
        if (start > head)
            start = head; // clock skew / stale cursor: nothing new
        const uint64_t available = head - start;
        if (available > kBufferBytes)
        {
            // Part of the requested data was overwritten by newer data.
            r.dropped = available - kBufferBytes;
            s_total_dropped += r.dropped;
            start = head - kBufferBytes;
        }
    }

    uint64_t avail = head - start;
    if (avail > cap)
        avail = cap;

    // Copy in at most two chunks (the buffer can wrap once inside the range).
    size_t copied = 0;
    uint64_t pos = start;
    while (copied < avail)
    {
        const uint64_t slot = pos & (kBufferBytes - 1);
        size_t chunk = kBufferBytes - slot; // contiguous run to the end of the ring
        if (chunk > avail - copied)
            chunk = static_cast<size_t>(avail - copied);
        std::memcpy(buf + copied, s_buf + slot, chunk);
        copied += chunk;
        pos += chunk;
    }

    r.bytes = copied;
    r.new_seq = start + copied;

    // Callers send r.bytes explicitly, so a NUL terminator is only a
    // convenience - and only when there is room for it (a full-cap read
    // would otherwise write one byte past the end of the buffer).
    if (copied < cap)
        buf[copied] = '\0';

    s_read_idx.store(r.new_seq, std::memory_order_release); // informational only
    return r;
}

uint64_t seq()
{
    return s_write_idx.load(std::memory_order_acquire);
}

uint64_t dropped_bytes()
{
    return s_total_dropped;
}

} // namespace serial_console

#endif
