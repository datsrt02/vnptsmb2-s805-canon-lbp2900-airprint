// SPDX-License-Identifier: GPL-3.0-or-later
// Canon LBP2900 CAPT/Hi-SCoA driver.
//
// Based on the public reverse engineering and Hi-SCoA work in captdriver:
// https://github.com/agalakhov/captdriver. This module is GPLv3-or-later;
// it contains no Canon binary code. Linking it into the firmware makes the
// resulting LBP2900 firmware image subject to GPLv3-or-later.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "printer_driver.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <errno.h>
#include <time.h>
#endif

#define CAPT_STATUS             0xe0a0
#define CAPT_XSTATUS            0xa0a8
#define CAPT_IDENT              0xa1a1
#define CAPT_JOB_BEGIN          0xa2a0
#define CAPT_START_0            0xa3a2
#define CAPT_PRINT_DATA         0xc0a0
#define CAPT_PRINT_DATA_END     0xc0a4
#define CAPT_SET_PAGE           0xd0a0
#define CAPT_SET_1              0xd0a1
#define CAPT_SET_2              0xd0a2
#define CAPT_SET_HISCOA         0xd0a4
#define CAPT_SET_ALL            0xd0a9
#define CAPT_START_2            0xe0a2
#define CAPT_START_1            0xe0a3
#define CAPT_START_3            0xe0a4
#define CAPT_UPLOAD_2           0xe0a5
#define CAPT_FIRE               0xe0a7
#define CAPT_JOB_END            0xe0a9
#define CAPT_JOB_SETUP          0xe1a1
#define CAPT_GPIO               0xe1a2

#define CAPT_BAND_LINES 70
#define CAPT_REPLY_MAX 256
#define CAPT_COMMAND_TIMEOUT_MS 15000
#define CAPT_PAGE_TIMEOUT_TICKS 3600 /* 6 minutes at 100 ms */

typedef struct {
    uint16_t flags[7];
    uint16_t decoding, printing, out, completed, received;
} capt_status_t;

typedef struct {
    printer_io_t io;
    int paper;
    int src_w, src_h, bpp;
    int line_bytes;
    int band_lines;
    int page_no;
    uint16_t job_id;
    bool live;                 // a real bidirectional link, not host capture
    bool page_open;
    int err;
    uint8_t *band;
    uint8_t *compressed;
    size_t compressed_cap;
    capt_status_t status;
} capt_job_t;

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void pause_100ms(void)
{
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(100));
#else
    struct timespec ts = {.tv_nsec = 100000000L};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
#endif
}

static void pause_1s(void)
{
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(1000));
#else
    struct timespec ts = {.tv_sec = 1};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
#endif
}

static size_t bcd_length(const uint8_t *p)
{
    if ((p[2] & 0x0f) > 9 || (p[2] >> 4) > 9 ||
        (p[3] & 0x0f) > 9 || (p[3] >> 4) > 9)
        return 0;
    return (size_t)(p[2] & 0x0f) + 10u * (p[2] >> 4) +
           100u * (p[3] & 0x0f) + 1000u * (p[3] >> 4);
}

static int write_packet(capt_job_t *j, uint16_t command, const void *payload, size_t payload_len)
{
    uint8_t header[4];
    put16(header, command);
    put16(header + 2, payload_len + sizeof(header));
    if (j->io.write(j->io.ctx, header, sizeof(header)) < 0 ||
        (payload_len && j->io.write(j->io.ctx, payload, payload_len) < 0)) {
        j->err = -1;
        return -1;
    }
    return 0;
}

// Host captures have no printer back channel. Emit the request and synthesize
// just enough reply state for deterministic .prn generation and unit tests.
static void offline_reply(capt_job_t *j, uint16_t command, uint8_t *reply, size_t *reply_len)
{
    size_t n = command == CAPT_STATUS || command == CAPT_XSTATUS ? 88 : command == CAPT_JOB_BEGIN ? 10 : 6;
    if (*reply_len < n) {
        *reply_len = 0;
        return;
    }
    memset(reply, 0, n);
    put16(reply, command);
    put16(reply + 2, n);
    if (command == CAPT_JOB_BEGIN)
        put16(reply + 6, 1);
    if (command == CAPT_STATUS || command == CAPT_XSTATUS) {
        put16(reply + 4 + 14, j->page_no);
        put16(reply + 4 + 16, j->page_no);
        put16(reply + 4 + 18, j->page_no);
        put16(reply + 4 + 20, j->page_no);
        put16(reply + 4 + 34, j->page_no);
    }
    *reply_len = n;
}

static int exchange(capt_job_t *j, uint16_t command, const void *payload, size_t payload_len,
                    uint8_t *reply, size_t *reply_len)
{
    if (payload_len > 120 || !reply || !reply_len)
        return j->err = -1;
    uint8_t request[124];
    put16(request, command);
    put16(request + 2, payload_len + 4);
    if (payload_len)
        memcpy(request + 4, payload, payload_len);

    if (j->io.transact) {
        if (j->io.transact(j->io.ctx, request, payload_len + 4, reply, reply_len,
                           CAPT_COMMAND_TIMEOUT_MS) < 0) {
            fprintf(stderr, "E capt: command 0x%04x transport failed\n", command);
            return j->err = -1;
        }
    } else {
        if (j->io.write(j->io.ctx, request, payload_len + 4) < 0)
            return j->err = -1;
        offline_reply(j, command, reply, reply_len);
    }
    if (*reply_len < 6 || u16(reply) != command ||
        (u16(reply + 2) != *reply_len && bcd_length(reply) != *reply_len)) {
        fprintf(stderr,
                "E capt: command 0x%04x invalid reply: cmd=0x%04x bytes=%zu binary=%u bcd=%zu\n",
                command, *reply_len >= 2 ? u16(reply) : 0, *reply_len,
                *reply_len >= 4 ? u16(reply + 2) : 0,
                *reply_len >= 4 ? bcd_length(reply) : 0);
        return j->err = -1;
    }
    return 0;
}

static int command_reply(capt_job_t *j, uint16_t command, const void *payload, size_t payload_len)
{
    uint8_t reply[CAPT_REPLY_MAX];
    size_t n = sizeof(reply);
    return exchange(j, command, payload, payload_len, reply, &n);
}

static int get_status(capt_job_t *j, uint16_t command)
{
    uint8_t reply[CAPT_REPLY_MAX];
    size_t n = sizeof(reply);
    if (exchange(j, command, NULL, 0, reply, &n) < 0)
        return -1;
    // CAPT_CHKSTATUS commonly returns only the two-byte base flag word. When
    // XSTATUS_CHNG is set, the following CAPT_CHKXSTATUS supplies the extended
    // counters. Decode every field that is actually present instead of
    // requiring a full extended-status packet.
    if (n < 6) {
        fprintf(stderr, "E capt: status 0x%04x has no flag payload\n", command);
        return j->err = -1;
    }
    const uint8_t *s = reply + 4;
    size_t size = n - 4;
    j->status.flags[0] = u16(s + 0);
    if (size >= 10) j->status.flags[1] = u16(s + 8);
    if (size >= 14) {
        j->status.flags[2] = u16(s + 10);
        j->status.flags[3] = u16(s + 12);
    }
    if (size >= 22) {
        j->status.decoding = u16(s + 14);
        j->status.printing = u16(s + 16);
        j->status.out = u16(s + 18);
        j->status.completed = u16(s + 20);
    }
    if (size >= 26) j->status.flags[4] = u16(s + 24);
    if (size >= 32) j->status.flags[5] = u16(s + 30);
    if (size >= 36) j->status.received = u16(s + 34);
    if (size >= 40) j->status.flags[6] = u16(s + 38);
    if (command == CAPT_XSTATUS) {
        fprintf(stderr,
                "I capt: status flags=%04x/%04x/%04x/%04x pages recv=%u decode=%u print=%u out=%u done=%u\n",
                j->status.flags[0], j->status.flags[1], j->status.flags[2], j->status.flags[3],
                j->status.received, j->status.decoding, j->status.printing,
                j->status.out, j->status.completed);
    }
    return 0;
}

static bool flag(const capt_job_t *j, int word, int bit)
{
    return (j->status.flags[word] & (1u << bit)) != 0;
}

// Extended status is only valid when the base status says it changed. The
// base packet also carries usable page counters, so callers always get a
// current status even when no A0A8 transaction is needed.
static int get_xstatus(capt_job_t *j)
{
    if (get_status(j, CAPT_STATUS) < 0)
        return -1;
    return flag(j, 0, 8) ? get_status(j, CAPT_XSTATUS) : 0;
}

static int wait_ready(capt_job_t *j)
{
    if (!j->live)
        return 0;
    for (int i = 0; i < 300; i++) {
        if (get_status(j, CAPT_STATUS) < 0)
            return -1;
        if (!flag(j, 0, 7))
            return 0;
        pause_100ms();
    }
    return j->err = -1;
}

static int job_setup(capt_job_t *j, uint8_t phase, uint16_t page)
{
    uint8_t p[72] = {0};
    put16(p + 4, page);
    p[16] = phase;
    p[17] = 1;
    put16(p + 18, j->job_id);
    // The original CAPT monitor sends these two signed timezone fields.
    p[20] = 0xc4; p[21] = 0xff;
    p[22] = 0x88; p[23] = 0xff;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) {
        put16(p + 24, (unsigned)tm->tm_year);
        p[26] = (uint8_t)tm->tm_mon;
        p[27] = (uint8_t)tm->tm_mday;
        p[28] = (uint8_t)tm->tm_hour;
        p[29] = (uint8_t)tm->tm_min;
        p[30] = (uint8_t)tm->tm_sec;
    }
    p[31] = 1;
    return command_reply(j, CAPT_JOB_SETUP, p, sizeof(p));
}

// -------------------------------------------------------------------------
// Hi-SCoA band compressor (MSB-first bitstream, XOR 0x43 obfuscation)

typedef struct {
    const uint8_t *in;
    size_t in_len, in_pos;
    uint8_t *out;
    size_t out_bits;
    size_t bit_pos;
    unsigned stride;
    uint8_t recent[16];
    unsigned recent_len;
    unsigned distance[6];
    bool overflow;
} bitstream_t;

static void bits_put(bitstream_t *b, uint32_t value, unsigned count)
{
    while (count) {
        if (b->bit_pos >= b->out_bits) {
            b->overflow = true;
            return;
        }
        unsigned byte = (unsigned)(b->bit_pos >> 3);
        unsigned room = 8 - (unsigned)(b->bit_pos & 7);
        unsigned take = count < room ? count : room;
        unsigned shift = room - take;
        uint8_t mask = (uint8_t)(((1u << take) - 1) << shift);
        uint8_t v = (uint8_t)((value >> (count - take)) << shift);
        uint8_t plain = b->out[byte] ^ 0x43;
        b->out[byte] = (uint8_t)(((plain & ~mask) | (v & mask)) ^ 0x43);
        b->bit_pos += take;
        count -= take;
    }
}

static unsigned bit_width(unsigned v)
{
    unsigned n = 0;
    do { n++; } while (v >>= 1);
    return n;
}

static size_t match_length(const bitstream_t *b, unsigned distance)
{
    if (!distance || b->in_pos < distance)
        return 0;
    size_t p = b->in_pos;
    size_t limit = p + 639;
    if (limit > b->in_len)
        limit = b->in_len;
    while (p < limit && p % b->stride && b->in[p] == b->in[p - distance])
        p++;
    // Position zero is also a line boundary, but a match may start there.
    if (p == b->in_pos) {
        while (p < limit && b->in[p] == b->in[p - distance]) {
            p++;
            if (p % b->stride == 0)
                break;
        }
    }
    return p - b->in_pos;
}

static void encode_length(bitstream_t *b, unsigned length)
{
    switch (length) {
    case 0: bits_put(b, 0x3f, 6); return;
    case 1: bits_put(b, 0, 2); return;
    case 2: bits_put(b, 3, 3); return;
    case 3: bits_put(b, 2, 3); return;
    default: {
        unsigned order = bit_width(length) - 1;
        bits_put(b, 0xfffffffeu, order);
        bits_put(b, ~length, order);
    }
    }
}

static bool encode_match(bitstream_t *b)
{
    unsigned best_slot = 0;
    size_t best = 0;
    for (unsigned slot = 0; slot < 6; slot++) {
        size_t n = match_length(b, b->distance[slot]);
        if (n > best) {
            best = n;
            best_slot = slot;
        }
    }
    if (best < 2)
        return false;

    unsigned tail = (unsigned)best;
    if (tail > 127) {
        unsigned blocks = tail / 128;
        unsigned order = bit_width(blocks) - 1;
        bits_put(b, 0xfc, 8);
        bits_put(b, order, 2);
        bits_put(b, ~blocks, order);
        tail %= 128;
    }
    bits_put(b, 0xfffffffeu, best_slot + 1);
    if (best_slot == 2)
        bits_put(b, 0, 1);
    encode_length(b, tail);
    if (best_slot == 2) {
        unsigned t = b->distance[2]; b->distance[2] = b->distance[0]; b->distance[0] = t;
    } else if (best_slot == 5) {
        unsigned t = b->distance[5]; b->distance[5] = b->distance[3]; b->distance[3] = t;
    }
    b->in_pos += best;
    return true;
}

static bool encode_recent(bitstream_t *b)
{
    uint8_t v = b->in[b->in_pos];
    for (unsigned i = 0; i < b->recent_len; i++) {
        if (b->recent[i] != v)
            continue;
        memmove(b->recent + 1, b->recent, i);
        b->recent[0] = v;
        bits_put(b, 0x20u | ((15 - i) & 15), 6);
        b->in_pos++;
        return true;
    }
    return false;
}

static void encode_literal(bitstream_t *b)
{
    uint8_t v = b->in[b->in_pos++];
    if (b->recent_len < 16)
        b->recent_len++;
    memmove(b->recent + 1, b->recent, b->recent_len - 1);
    b->recent[0] = v;
    if (v == 0)
        bits_put(b, 0xfd, 8);
    else
        bits_put(b, 0xd00u | v, 12);
}

static size_t hiscoa_compress(uint8_t *out, size_t out_len, const uint8_t *in,
                              unsigned stride, unsigned lines)
{
    memset(out, 0, out_len);
    bitstream_t b = {
        .in = in, .in_len = (size_t)stride * lines,
        .out = out, .out_bits = out_len * 8, .stride = stride,
        .distance = {stride, 0, stride - 7, 1, 0, 4},
    };
    while (b.in_pos < b.in_len && !b.overflow) {
        if (!encode_match(&b) && !encode_recent(&b))
            encode_literal(&b);
    }
    bits_put(&b, 0xfe, 8);
    bits_put(&b, 0, 2); // ordinary end-of-band
    bits_put(&b, 0xffffffffu, 32 - (unsigned)(b.bit_pos & 31));
    return b.overflow ? 0 : b.bit_pos / 8;
}

// -------------------------------------------------------------------------

static int send_band(capt_job_t *j)
{
    if (!j->band_lines)
        return 0;
    size_t n = hiscoa_compress(j->compressed, j->compressed_cap, j->band,
                               (unsigned)j->line_bytes, (unsigned)j->band_lines);
    if (!n)
        return j->err = -1;
    const uint8_t *p = j->compressed;
    while (n) {
        size_t chunk = n > 0xff00 ? 0xff00 : n;
        if (write_packet(j, CAPT_PRINT_DATA, p, chunk) < 0)
            return -1;
        p += chunk;
        n -= chunk;
    }
    j->band_lines = 0;
    memset(j->band, 0, (size_t)j->line_bytes * CAPT_BAND_LINES);
    return 0;
}

static int send_page_params(capt_job_t *j)
{
    uint8_t page[40] = {
        0, 0, 0x30, 0x2a, (uint8_t)(j->paper == 0 ? 0x02 : 0x0d), 0, 0, 0,
        0x1c, 0x1c, 0x1c, 0x1c, 0, 0x11, 4, 0,
        1, 1, 2, 0, 0, 0,
    };
    put16(page + 22, 0); // top and left margins; raster is already centered
    put16(page + 24, 0);
    put16(page + 26, (unsigned)j->line_bytes);
    put16(page + 28, (unsigned)j->src_h);
    put16(page + 30, (unsigned)j->src_w);
    put16(page + 32, (unsigned)j->src_h);
    page[36] = 1;       // plain-paper fuser mode

    const uint8_t hiscoa[8] = {1, 4, 1, 1, 0, (uint8_t)-7, 0, 0};
    uint8_t all[68];
    size_t at = 0;
    put16(all + at, CAPT_SET_ALL); put16(all + at + 2, sizeof(all)); at += 4;
#define ADD_SUBCOMMAND(cmd, data, len) do {                   \
        put16(all + at, (cmd)); put16(all + at + 2, (len) + 4); \
        if ((len) != 0) memcpy(all + at + 4, (data), (len));   \
        at += (len) + 4;                                       \
    } while (0)
    ADD_SUBCOMMAND(CAPT_SET_PAGE, page, sizeof(page));
    ADD_SUBCOMMAND(CAPT_SET_HISCOA, hiscoa, sizeof(hiscoa));
    ADD_SUBCOMMAND(CAPT_SET_1, NULL, 0);
    ADD_SUBCOMMAND(CAPT_SET_2, NULL, 0);
#undef ADD_SUBCOMMAND
    if (at != sizeof(all) || j->io.write(j->io.ctx, all, sizeof(all)) < 0)
        return j->err = -1;
    return 0;
}

static void *job_begin(const printer_driver_t *d, const printer_io_t *io, int dpi, int paper)
{
    (void)d;
    if (dpi != 600 || paper < 0 || paper > 1)
        return NULL;
    capt_job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->io = *io;
    j->paper = paper;
    j->live = io->transact != NULL;

    uint8_t reply[CAPT_REPLY_MAX];
    size_t n = sizeof(reply);
    static const uint8_t begin_magic[8] = {0, 0, 0x1e, 0, 0, 0, 0, 0};
    static const uint8_t gpio_init[16] = {0x13};
    if (exchange(j, CAPT_IDENT, NULL, 0, reply, &n) < 0) {
        free(j);
        return NULL;
    }
    pause_1s();
    // START_0 is a request/reply command. Leaving its six-byte acknowledgement
    // unread shifts the back channel and makes JOB_BEGIN consume the wrong
    // response.
    if (get_xstatus(j) < 0 || command_reply(j, CAPT_START_0, NULL, 0) < 0) {
        free(j);
        return NULL;
    }
    n = sizeof(reply);
    if (exchange(j, CAPT_JOB_BEGIN, begin_magic, sizeof(begin_magic), reply, &n) < 0 || n < 8) {
        free(j);
        return NULL;
    }
    j->job_id = u16(reply + 6);
    if (command_reply(j, CAPT_GPIO, gpio_init, sizeof(gpio_init)) < 0 ||
        wait_ready(j) < 0 || job_setup(j, 1, 0) < 0 || wait_ready(j) < 0) {
        free(j);
        return NULL;
    }
    return j;
}

static int page_begin(void *job, int src_w, int src_h, int bytes_pp)
{
    capt_job_t *j = job;
    if (j->err || j->page_open || (bytes_pp != 1 && bytes_pp != 3) || src_w <= 0 || src_h <= 0)
        return -1;
    j->src_w = src_w;
    j->src_h = src_h;
    j->bpp = bytes_pp;
    j->line_bytes = j->paper == 0 ? 595 : 612; // CAPT's 600-dpi A4/Letter scanline
    size_t band_size = (size_t)j->line_bytes * CAPT_BAND_LINES;
    j->compressed_cap = band_size * 2 + 16;
    j->band = calloc(1, band_size);
    j->compressed = malloc(j->compressed_cap);
    if (!j->band || !j->compressed)
        return j->err = -1;

    if (j->live && get_xstatus(j) < 0)
        return -1;
    if (j->live && (flag(j, 0, 5) || flag(j, 0, 4))) {
        static const uint8_t upload[16] = {0xee, 0xdb, 0xea, 0xad};
        if (command_reply(j, CAPT_START_1, NULL, 0) < 0 ||
            command_reply(j, CAPT_START_2, NULL, 0) < 0 ||
            command_reply(j, CAPT_START_3, NULL, 0) < 0 || wait_ready(j) < 0 ||
            command_reply(j, CAPT_UPLOAD_2, upload, sizeof(upload)) < 0 || wait_ready(j) < 0)
            return -1;
    }
    if (send_page_params(j) < 0)
        return -1;
    j->page_open = true;
    return 0;
}

static const uint8_t BAYER8[64] = {
     0,32, 8,40, 2,34,10,42, 48,16,56,24,50,18,58,26,
    12,44, 4,36,14,46, 6,38, 60,28,52,20,62,30,54,22,
     3,35,11,43, 1,33, 9,41, 51,19,59,27,49,17,57,25,
    15,47, 7,39,13,45, 5,37, 63,31,55,23,61,29,53,21,
};

static int page_line(void *job, int y, const uint8_t *px)
{
    capt_job_t *j = job;
    if (j->err || !j->page_open || y < 0 || y >= j->src_h)
        return -1;
    uint8_t *dst = j->band + (size_t)j->band_lines * j->line_bytes;
    int dst_pixels = j->line_bytes * 8;
    int copy = j->src_w < dst_pixels ? j->src_w : dst_pixels;
    int src_x = (j->src_w - copy) / 2;
    int dst_x = (dst_pixels - copy) / 2;
    if (px) {
        for (int x = 0; x < copy; x++) {
            int sx = src_x + x;
            unsigned gray;
            if (j->bpp == 1)
                gray = px[sx];
            else {
                const uint8_t *rgb = px + (size_t)sx * 3;
                gray = (77u * rgb[0] + 150u * rgb[1] + 29u * rgb[2]) >> 8;
            }
            unsigned threshold = (unsigned)BAYER8[((unsigned)y & 7) * 8 + ((unsigned)(dst_x + x) & 7)] * 4 + 2;
            if (gray < threshold)
                dst[(dst_x + x) >> 3] |= (uint8_t)(0x80u >> ((dst_x + x) & 7));
        }
    }
    j->band_lines++;
    return j->band_lines == CAPT_BAND_LINES ? send_band(j) : 0;
}

static int wait_for_page(capt_job_t *j, int which, uint16_t page)
{
    if (!j->live)
        return 0;
    for (int i = 0; i < CAPT_PAGE_TIMEOUT_TICKS; i++) {
        if (get_xstatus(j) < 0)
            return -1;
        uint16_t value = which == 0 ? j->status.received : which == 1 ? j->status.out : j->status.completed;
        if (value == page)
            return 0;
        // Match captdriver's LBP2900 behavior: once the printer has stopped
        // processing and still reports an empty paper source, fail the page
        // instead of keeping the IPP connection blocked for six minutes.
        if (which == 1 && (flag(j, 0, 1) || flag(j, 1, 14)) &&
            !flag(j, 1, 2) && !flag(j, 1, 7)) {
            fprintf(stderr, "E capt: no paper\n");
            return j->err = -1;
        }
        pause_100ms();
    }
    return j->err = -1;
}

static int page_end(void *job)
{
    capt_job_t *j = job;
    if (j->err || !j->page_open || send_band(j) < 0 || write_packet(j, CAPT_PRINT_DATA_END, NULL, 0) < 0)
        return -1;
    j->page_no++;
    uint16_t page = (uint16_t)j->page_no;
    if (j->live) {
        // The printer assigns/echoes the decoding page number in status.
        if (wait_for_page(j, 0, page) < 0)
            return -1;
        page = j->status.decoding;
    }
    uint8_t p[2]; put16(p, page);
    if (job_setup(j, 2, page) < 0 || wait_ready(j) < 0 ||
        command_reply(j, CAPT_FIRE, p, sizeof(p)) < 0 || wait_ready(j) < 0 ||
        job_setup(j, 6, page) < 0 || wait_for_page(j, 1, page) < 0)
        return -1;
    free(j->band); j->band = NULL;
    free(j->compressed); j->compressed = NULL;
    j->page_open = false;
    return 0;
}

static int job_end(void *job)
{
    capt_job_t *j = job;
    if (j->page_open && !j->err)
        page_end(j);
    uint16_t page = (uint16_t)j->page_no;
    if (!j->err && wait_for_page(j, 2, page) == 0 && job_setup(j, 4, page) == 0) {
        uint8_t p[2]; put16(p, j->job_id);
        command_reply(j, CAPT_JOB_END, p, sizeof(p));
    }
    int ret = j->err ? -1 : 0;
    free(j->band);
    free(j->compressed);
    free(j);
    return ret;
}

static const printer_paper_t PAPERS[] = {
    {"A4", "iso_a4_210x297mm", 21000, 29700},
    {"Letter", "na_letter_8.5x11in", 21590, 27940},
};

const printer_driver_t canon_lbp2900_driver = {
    .id = "canon-lbp2900",
    .make_model = "Canon LBP2900",
    .usb_mfg = "Canon",
    .usb_mdl = "LBP2900",
    .usb_vid = 0x04a9,
    .usb_pid = 0x2676,
    .color = false,
    .scanner = false,
    .resolutions = {600},
    .nresolutions = 1,
    .papers = PAPERS,
    .npapers = 2,
    .margin_left = 500,
    .margin_right = 500,
    .margin_top = 500,
    .margin_bottom = 500,
    .job_begin = job_begin,
    .page_begin = page_begin,
    .page_line = page_line,
    .page_end = page_end,
    .job_end = job_end,
};
