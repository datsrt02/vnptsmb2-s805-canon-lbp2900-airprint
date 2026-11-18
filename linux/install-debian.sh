#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root: sudo sh linux/install-debian.sh" >&2
    exit 1
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PREBUILT="$ROOT/linux/lbp2900-airprint-armhf.bin"
ARCH=$(dpkg --print-architecture 2>/dev/null || uname -m)

if ! command -v start-stop-daemon >/dev/null 2>&1; then
    echo "Missing start-stop-daemon (package dpkg)." >&2
    exit 1
fi

case "$ARCH" in
    armhf|armv7l|armv7*)
        if [ -f "$PREBUILT" ]; then
            BINARY=$PREBUILT
            echo "Using bundled static ARMHF binary."
        fi
        ;;
esac

if [ -z "${BINARY:-}" ]; then
    if ! command -v make >/dev/null 2>&1 || ! command -v cc >/dev/null 2>&1; then
        echo "No compatible prebuilt binary and no compiler." >&2
        echo "Install build-essential, then run this installer again." >&2
        exit 1
    fi
    make -C "$ROOT/linux" clean all
    BINARY="$ROOT/linux/lbp2900-airprint"
fi

install -D -m 0755 "$BINARY" /usr/local/sbin/lbp2900-airprint
install -D -m 0755 "$ROOT/linux/lbp2900-airprint.init" /etc/init.d/lbp2900-airprint
install -D -m 0644 "$ROOT/linux/lbp2900-airprint.default" /etc/default/lbp2900-airprint
install -D -m 0644 "$ROOT/linux/lbp2900-airprint.service" /etc/avahi/services/lbp2900-airprint.service
install -d -m 0755 /var/lib/lbp2900-airprint

update-rc.d lbp2900-airprint defaults
service lbp2900-airprint restart
if [ -x /etc/init.d/avahi-daemon ]; then
    service avahi-daemon restart
else
    echo "Warning: avahi-daemon is not installed; AirPrint auto-discovery will not work." >&2
fi

echo "Installed. Web status: http://<box-ip>:8080/"
echo "IPP endpoint: ipp://<box-ip>:631/ipp/print"
echo "Check: service lbp2900-airprint status"
