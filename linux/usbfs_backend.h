#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>

typedef struct {
    pthread_mutex_t job_lock;
    int fd;
    int interface_number;
    int alternate_setting;
    bool kernel_driver_detached;
    uint8_t endpoint_out;
    uint8_t endpoint_in;
    atomic_bool active;
    atomic_bool connected;
    atomic_bool cancel_requested;
} usbfs_backend_t;

void usbfs_backend_init(usbfs_backend_t *b);
bool usbfs_backend_present(usbfs_backend_t *b);
int usbfs_backend_job_begin(usbfs_backend_t *b);
int usbfs_backend_write(usbfs_backend_t *b, const uint8_t *data, size_t len);
int usbfs_backend_transact(usbfs_backend_t *b, const uint8_t *request, size_t request_len,
                           uint8_t *reply, size_t *reply_len, uint32_t timeout_ms);
void usbfs_backend_cancel(usbfs_backend_t *b);
void usbfs_backend_job_end(usbfs_backend_t *b, bool ok);
