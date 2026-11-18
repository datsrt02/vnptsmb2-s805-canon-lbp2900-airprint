#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "usbfs_backend.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usbdevice_fs.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CANON_VENDOR_ID  0x04a9
#define LBP2900_PRODUCT_ID 0x2676
#define USB_IO_TIMEOUT_MS 15000
#define MAX_CONFIG_SIZE 4096

typedef struct {
    int interface_number;
    int alternate_setting;
    uint8_t endpoint_out;
    uint8_t endpoint_in;
    int rank;
} printer_interface_t;

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static bool numeric_name(const char *s)
{
    if (!s || !*s)
        return false;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s))
            return false;
    return true;
}

static int control_transfer(int fd, uint8_t request_type, uint8_t request,
                            uint16_t value, uint16_t index, void *data,
                            uint16_t length, unsigned timeout_ms)
{
    struct usbdevfs_ctrltransfer x = {
        .bRequestType = request_type,
        .bRequest = request,
        .wValue = value,
        .wIndex = index,
        .wLength = length,
        .timeout = timeout_ms,
        .data = data,
    };
    return ioctl(fd, USBDEVFS_CONTROL, &x);
}

static int get_descriptor(int fd, uint8_t type, uint8_t index, void *data, uint16_t length)
{
    return control_transfer(fd, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                            USB_REQ_GET_DESCRIPTOR, (uint16_t)type << 8 | index,
                            0, data, length, 2000);
}

static bool find_printer_interface(const uint8_t *cfg, size_t size, printer_interface_t *best)
{
    memset(best, 0, sizeof(*best));
    best->rank = -1;
    for (size_t pos = 0; pos + 2 <= size;) {
        uint8_t len = cfg[pos];
        uint8_t type = cfg[pos + 1];
        if (len < 2 || pos + len > size)
            break;
        if (type == USB_DT_INTERFACE && len >= USB_DT_INTERFACE_SIZE) {
            const uint8_t *i = cfg + pos;
            if (i[5] == USB_CLASS_PRINTER && i[6] == 1) {
                uint8_t ep_out = 0, ep_in = 0;
                size_t next = pos + len;
                while (next + 2 <= size) {
                    uint8_t dlen = cfg[next];
                    uint8_t dtype = cfg[next + 1];
                    if (dlen < 2 || next + dlen > size || dtype == USB_DT_INTERFACE)
                        break;
                    if (dtype == USB_DT_ENDPOINT && dlen >= USB_DT_ENDPOINT_SIZE &&
                        (cfg[next + 3] & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK) {
                        uint8_t ep = cfg[next + 2];
                        if (ep & USB_DIR_IN)
                            ep_in = ep;
                        else
                            ep_out = ep;
                    }
                    next += dlen;
                }
                int protocol = i[7];
                int rank = protocol == 2 ? 3 : protocol == 1 ? 2 : protocol == 3 ? 1 : 0;
                if (ep_out && ep_in && rank > best->rank) {
                    best->interface_number = i[2];
                    best->alternate_setting = i[3];
                    best->endpoint_out = ep_out;
                    best->endpoint_in = ep_in;
                    best->rank = rank;
                }
            }
        }
        pos += len;
    }
    return best->rank >= 0;
}

static int inspect_device(int fd, printer_interface_t *pi)
{
    uint8_t dev[USB_DT_DEVICE_SIZE];
    if (get_descriptor(fd, USB_DT_DEVICE, 0, dev, sizeof(dev)) != (int)sizeof(dev) ||
        get_u16(dev + 8) != CANON_VENDOR_ID || get_u16(dev + 10) != LBP2900_PRODUCT_ID)
        return -1;

    uint8_t head[USB_DT_CONFIG_SIZE];
    if (get_descriptor(fd, USB_DT_CONFIG, 0, head, sizeof(head)) != (int)sizeof(head))
        return -1;
    size_t total = get_u16(head + 2);
    if (total < sizeof(head) || total > MAX_CONFIG_SIZE)
        return -1;
    uint8_t *cfg = malloc(total);
    if (!cfg)
        return -1;
    int got = get_descriptor(fd, USB_DT_CONFIG, 0, cfg, (uint16_t)total);
    bool ok = got >= (int)sizeof(head) && find_printer_interface(cfg, (size_t)got, pi);
    free(cfg);
    return ok ? 0 : -1;
}

static int open_lbp2900(printer_interface_t *pi, char *chosen, size_t chosen_size)
{
    DIR *root = opendir("/dev/bus/usb");
    if (!root)
        return -1;
    int result = -1;
    struct dirent *bus;
    while (result < 0 && (bus = readdir(root)) != NULL) {
        if (!numeric_name(bus->d_name))
            continue;
        char bus_path[256];
        snprintf(bus_path, sizeof(bus_path), "/dev/bus/usb/%s", bus->d_name);
        DIR *devices = opendir(bus_path);
        if (!devices)
            continue;
        struct dirent *dev;
        while ((dev = readdir(devices)) != NULL) {
            if (!numeric_name(dev->d_name))
                continue;
            char path[320];
            snprintf(path, sizeof(path), "%s/%s", bus_path, dev->d_name);
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd < 0)
                continue;
            if (inspect_device(fd, pi) == 0) {
                snprintf(chosen, chosen_size, "%s", path);
                result = fd;
                break;
            }
            close(fd);
        }
        closedir(devices);
    }
    closedir(root);
    return result;
}

static void reconnect_kernel_driver(int fd, int interface_number)
{
    struct usbdevfs_ioctl command = {
        .ifno = interface_number,
        .ioctl_code = USBDEVFS_CONNECT,
        .data = NULL,
    };
    (void)ioctl(fd, USBDEVFS_IOCTL, &command);
}

static int claim_interface(int fd, const printer_interface_t *pi, bool *detached)
{
    int interface_number = pi->interface_number;
    *detached = false;
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &interface_number) == 0)
        goto claimed;
    if (errno == EBUSY) {
        struct usbdevfs_ioctl command = {
            .ifno = interface_number,
            .ioctl_code = USBDEVFS_DISCONNECT,
            .data = NULL,
        };
        if (ioctl(fd, USBDEVFS_IOCTL, &command) == 0)
            *detached = true;
        if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &interface_number) < 0) {
            if (*detached)
                reconnect_kernel_driver(fd, interface_number);
            return -1;
        }
    } else {
        return -1;
    }
claimed:
    if (pi->alternate_setting) {
        struct usbdevfs_setinterface set = {
            .interface = (unsigned)pi->interface_number,
            .altsetting = (unsigned)pi->alternate_setting,
        };
        if (ioctl(fd, USBDEVFS_SETINTERFACE, &set) < 0) {
            (void)ioctl(fd, USBDEVFS_RELEASEINTERFACE, &interface_number);
            if (*detached)
                reconnect_kernel_driver(fd, interface_number);
            return -1;
        }
    }
    return 0;
}

static void close_device(usbfs_backend_t *b)
{
    if (b->fd < 0)
        return;
    if (b->interface_number >= 0) {
        int interface_number = b->interface_number;
        (void)ioctl(b->fd, USBDEVFS_RELEASEINTERFACE, &interface_number);
        if (b->kernel_driver_detached)
            reconnect_kernel_driver(b->fd, interface_number);
    }
    close(b->fd);
    b->fd = -1;
    b->interface_number = -1;
    b->kernel_driver_detached = false;
    b->endpoint_out = b->endpoint_in = 0;
}

static int bulk_transfer(int fd, uint8_t endpoint, void *data, size_t length, unsigned timeout_ms)
{
    if (length > (size_t)INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    struct usbdevfs_bulktransfer x = {
        .ep = endpoint,
        .len = (unsigned)length,
        .timeout = timeout_ms,
        .data = data,
    };
    return ioctl(fd, USBDEVFS_BULK, &x);
}

static int write_all(usbfs_backend_t *b, const uint8_t *data, size_t len)
{
    while (len) {
        if (atomic_load(&b->cancel_requested))
            return -1;
        size_t chunk = len > 16384 ? 16384 : len;
        int sent = bulk_transfer(b->fd, b->endpoint_out, (void *)data, chunk, USB_IO_TIMEOUT_MS);
        if (sent <= 0) {
            if (errno == ENODEV || errno == ESHUTDOWN)
                atomic_store(&b->connected, false);
            return -1;
        }
        data += sent;
        len -= (size_t)sent;
    }
    return 0;
}

static size_t bcd_length(const uint8_t *p)
{
    if ((p[2] & 15) > 9 || (p[2] >> 4) > 9 ||
        (p[3] & 15) > 9 || (p[3] >> 4) > 9)
        return 0;
    return (size_t)(p[2] & 15) + 10u * (p[2] >> 4) +
           100u * (p[3] & 15) + 1000u * (p[3] >> 4);
}

void usbfs_backend_init(usbfs_backend_t *b)
{
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->job_lock, NULL);
    b->fd = -1;
    b->interface_number = -1;
}

bool usbfs_backend_present(usbfs_backend_t *b)
{
    if (atomic_load(&b->active))
        return atomic_load(&b->connected);
    printer_interface_t pi;
    char path[320];
    int fd = open_lbp2900(&pi, path, sizeof(path));
    if (fd < 0) {
        atomic_store(&b->connected, false);
        return false;
    }
    close(fd);
    atomic_store(&b->connected, true);
    return true;
}

int usbfs_backend_job_begin(usbfs_backend_t *b)
{
    pthread_mutex_lock(&b->job_lock);
    printer_interface_t pi;
    char path[320];
    b->fd = open_lbp2900(&pi, path, sizeof(path));
    bool detached = false;
    if (b->fd < 0 || claim_interface(b->fd, &pi, &detached) < 0) {
        fprintf(stderr, "E usbfs: Canon LBP2900 04a9:2676 not available or interface busy\n");
        close_device(b);
        atomic_store(&b->connected, false);
        pthread_mutex_unlock(&b->job_lock);
        return -1;
    }
    b->interface_number = pi.interface_number;
    b->alternate_setting = pi.alternate_setting;
    b->kernel_driver_detached = detached;
    b->endpoint_out = pi.endpoint_out;
    b->endpoint_in = pi.endpoint_in;
    atomic_store(&b->cancel_requested, false);
    atomic_store(&b->connected, true);
    atomic_store(&b->active, true);
    fprintf(stderr, "I usbfs: using %s interface %d, bulk OUT 0x%02x, IN 0x%02x\n",
            path, b->interface_number, b->endpoint_out, b->endpoint_in);
    return 0;
}

int usbfs_backend_write(usbfs_backend_t *b, const uint8_t *data, size_t len)
{
    return atomic_load(&b->active) && data ? write_all(b, data, len) : -1;
}

int usbfs_backend_transact(usbfs_backend_t *b, const uint8_t *request, size_t request_len,
                           uint8_t *reply, size_t *reply_len, uint32_t timeout_ms)
{
    if (!atomic_load(&b->active) || !request || request_len < 4 || !reply || !reply_len || *reply_len < 6)
        return -1;
    uint16_t command = get_u16(request);
    if (write_all(b, request, request_len) < 0) {
        fprintf(stderr, "E usbfs: CAPT 0x%04x write failed: %s\n", command, strerror(errno));
        return -1;
    }

    size_t capacity = *reply_len;
    // A previously interrupted CAPT exchange can leave one or more complete
    // replies queued on bulk IN. Discard those packets until the reply command
    // matches the request just written, which resynchronizes without requiring
    // a physical USB power cycle.
    for (int attempt = 0; attempt < 8; attempt++) {
        int got = bulk_transfer(b->fd, b->endpoint_in, reply, capacity, timeout_ms);
        if (got < 6) {
            fprintf(stderr, "E usbfs: CAPT 0x%04x short/read failure: got=%d errno=%d (%s)\n",
                    command, got, errno, strerror(errno));
            return -1;
        }
        size_t have = (size_t)got;
        for (;;) {
            size_t binary = get_u16(reply + 2);
            size_t bcd = bcd_length(reply);
            if (have == binary || (bcd >= 6 && have == bcd))
                break;
            if (have >= capacity || binary < 6 || binary > capacity) {
                fprintf(stderr,
                        "E usbfs: CAPT 0x%04x invalid length: have=%zu binary=%zu bcd=%zu capacity=%zu\n",
                        command, have, binary, bcd, capacity);
                return -1;
            }
            got = bulk_transfer(b->fd, b->endpoint_in, reply + have, capacity - have, timeout_ms);
            if (got <= 0) {
                fprintf(stderr, "E usbfs: CAPT 0x%04x continuation failed: got=%d errno=%d (%s)\n",
                        command, got, errno, strerror(errno));
                return -1;
            }
            have += (size_t)got;
        }
        uint16_t reply_command = get_u16(reply);
        if (reply_command == command) {
            *reply_len = have;
            return 0;
        }
        fprintf(stderr,
                "W usbfs: discarding stale CAPT reply 0x%04x (%zu bytes), waiting for 0x%04x\n",
                reply_command, have, command);
    }
    errno = EPROTO;
    fprintf(stderr, "E usbfs: CAPT 0x%04x could not resynchronize reply stream\n", command);
    return -1;
}

void usbfs_backend_cancel(usbfs_backend_t *b)
{
    atomic_store(&b->cancel_requested, true);
}

void usbfs_backend_job_end(usbfs_backend_t *b, bool ok)
{
    if (!atomic_load(&b->active))
        return;
    if (!ok && b->fd >= 0) {
        (void)control_transfer(b->fd, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                               2, 0, (uint16_t)b->interface_number, NULL, 0, 2000);
    }
    close_device(b);
    atomic_store(&b->active, false);
    pthread_mutex_unlock(&b->job_lock);
}
