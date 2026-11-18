#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "airprint.h"
#include "events.h"
#include "printer_driver.h"
#include "usbfs_backend.h"

static usbfs_backend_t s_usb;
static volatile sig_atomic_t s_stop;

static void add_reason(airprint_status_t *st, const char *reason)
{
    if (st->nreasons < (int)(sizeof(st->reasons) / sizeof(st->reasons[0])))
        st->reasons[st->nreasons++] = reason;
}

static void get_status(void *ctx, airprint_status_t *st)
{
    usbfs_backend_t *usb = ctx;
    st->connected = usbfs_backend_present(usb);
    st->busy = atomic_load(&usb->active);
    st->nsupplies = 1;
    st->supply[0].name = "Black toner";
    st->supply[0].color = "#000000";
    st->supply[0].level = -2;
    if (!st->connected) {
        add_reason(st, "offline-report");
        snprintf(st->message, sizeof(st->message), "Canon LBP2900 USB not connected");
    }
}

static int job_open(void *ctx)
{
    return usbfs_backend_job_begin(ctx);
}

static int job_write(void *ctx, const uint8_t *data, size_t len)
{
    return usbfs_backend_write(ctx, data, len);
}

static int job_sync(void *ctx, const char *what)
{
    (void)ctx;
    (void)what;
    return 0;
}

static int job_transact(void *ctx, const uint8_t *request, size_t request_len,
                        uint8_t *reply, size_t *reply_len, uint32_t timeout_ms)
{
    return usbfs_backend_transact(ctx, request, request_len, reply, reply_len, timeout_ms);
}

static void job_close(void *ctx, bool ok)
{
    usbfs_backend_job_end(ctx, ok);
}

static void job_cancel(void *ctx)
{
    usbfs_backend_cancel(ctx);
}

static void make_uuid(char out[40])
{
    char seed[256] = "lbp2900-airprint";
    FILE *f = fopen("/etc/machine-id", "r");
    if (f) {
        if (!fgets(seed, sizeof(seed), f))
            strcpy(seed, "lbp2900-airprint");
        fclose(f);
    } else {
        (void)gethostname(seed, sizeof(seed) - 1);
        seed[sizeof(seed) - 1] = 0;
    }
    uint64_t a = UINT64_C(1469598103934665603);
    uint64_t b = UINT64_C(1099511628211);
    for (const unsigned char *p = (const unsigned char *)seed; *p; p++) {
        a = (a ^ *p) * UINT64_C(1099511628211);
        b = (b + *p) * UINT64_C(1469598103934665603);
    }
    snprintf(out, 40, "%08x-%04x-%04x-%04x-%012llx",
             (unsigned)(a >> 32), (unsigned)(a >> 16) & 0xffff,
             ((unsigned)a & 0x0fff) | 0x4000,
             ((unsigned)(b >> 48) & 0x3fff) | 0x8000,
             (unsigned long long)(b & UINT64_C(0xffffffffffff)));
}

static int env_port(const char *name, int fallback)
{
    const char *value = getenv(name);
    if (!value || !*value)
        return fallback;
    char *end;
    long port = strtol(value, &end, 10);
    return *end || port < 0 || port > 65535 ? fallback : (int)port;
}

static void on_signal(int sig)
{
    (void)sig;
    s_stop = 1;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s\n"
            "Environment: AIRPRINT_NAME, AIRPRINT_PORT (631), AIRPRINT_WEB_PORT (8080)\n",
            program);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        usage(argv[0]);
        return !(!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"));
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    usbfs_backend_init(&s_usb);

    const printer_driver_t *driver = printer_driver_find("canon-lbp2900");
    if (!driver) {
        fprintf(stderr, "E main: canon-lbp2900 driver is not linked\n");
        return 1;
    }
    static char uuid[40];
    make_uuid(uuid);
    const char *name = getenv("AIRPRINT_NAME");
    if (!name || !*name)
        name = "Canon LBP2900";

    static airprint_config_t cfg;
    cfg = (airprint_config_t){
        .name = name,
        .make_model = "Canon LBP2900",
        .uuid = uuid,
        .device_id = "MFG:Canon;MDL:LBP2900;CMD:CAPT;CLS:PRINTER;",
        .port = env_port("AIRPRINT_PORT", 631),
        .web_port = env_port("AIRPRINT_WEB_PORT", 8080),
        .driver = driver,
        .get_status = get_status,
        .job_open = job_open,
        .job_write = job_write,
        .job_sync = job_sync,
        .job_transact = job_transact,
        .job_close = job_close,
        .job_cancel = job_cancel,
        .ctx = &s_usb,
    };
    events_add_source("printer", airprint_status_json, NULL);
    if (airprint_start(&cfg) != 0 || events_start() != 0) {
        fprintf(stderr, "E main: cannot start IPP server (ports %d/%d, errno %d)\n",
                cfg.port, cfg.web_port, errno);
        return 1;
    }
    fprintf(stderr, "I main: %s, IPP %d, web %d, UUID %s\n",
            cfg.name, cfg.port, cfg.web_port, cfg.uuid);
    fprintf(stderr, "I main: waiting for Canon LBP2900 USB 04a9:2676\n");
    while (!s_stop)
        pause();
    fprintf(stderr, "I main: stopping\n");
    return 0;
}
