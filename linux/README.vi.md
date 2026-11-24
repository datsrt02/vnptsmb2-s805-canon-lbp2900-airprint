# Bản Linux ARMHF cho VNPT SmartBox 2 S805

Thư mục này port trực tiếp lõi IPP/AirPrint và driver CAPT của dự án sang
Linux. Nó **không chạy firmware ESP32**, không cần `systemd` và không cần
CUPS. USB được điều khiển hai chiều qua Linux USBFS để giữ nguyên các giao
dịch CAPT mà LBP2900 yêu cầu.

## Trạng thái

- Kiến trúc đích: Debian 11 ARMHF, Linux 3.10.33, SysV init.
- Máy in: Canon LBP2900/LBP2900B USB `04a9:2676`.
- In: đơn sắc, 600 dpi, A4/Letter, URF hoặc PWG Raster qua IPP.
- Copies: 1–99; với nhiều bản sao, server lưu tạm đúng một raster document rồi
  phát lại trong cùng IPP job.
- Khám phá iPhone/macOS: Avahi `_ipp._tcp`, subtype `_universal`.
- Không có: scan, duplex, IPPS, xác thực, PDF/JPEG trực tiếp qua IPP.
- Đã in thành công ba Windows Test Page liên tiếp trên Canon LBP2900 thật ngày
  19/11/2026. Mỗi IPP job chạy lại chuỗi khởi tạo CAPT để reset bộ đếm feed/page,
  tránh lỗi `NOPAPER` giả từng xuất hiện ở job thứ hai. Bản hiện tại chỉ quảng
  bá URF/PWG Raster và có cơ chế hủy job mồ côi; sau khi cập nhật cần xóa/thêm
  lại hàng đợi IPP trên Windows.
- Ngày 24/11/2026 đã kiểm thử phần cứng một trang với `copies=3`; CAPT hoàn tất
  ba lần kéo giấy và web báo `job-completed-successfully`, `3 page(s)`.
- Driver đợi CAPT báo tờ trước đã `completed` rồi mới khởi tạo lại engine cho
  tờ kế tiếp. Kiểm thử `copies=2` kết thúc đủ hai trang, không còn treo ở bản 2.

## Chuẩn bị image S805

Rootfs gốc chỉ khoảng 550 MiB và gần đầy. Hãy mở rộng ít nhất 1 GiB để còn chỗ
cho Avahi, log và hàng đợi in. Bộ phát hành có binary ARMHF tĩnh nên không cần
cài compiler trên box. Sửa APT source Bullseye archive nếu cần rồi cài:

```sh
apt-get update
apt-get install avahi-daemon
```

Kernel phải có USB host/usbfs và nhìn thấy máy in:

```sh
lsusb -d 04a9:2676
ls -l /dev/bus/usb/*/*
```

## Cài bản đã build sẵn

```sh
sudo sh linux/install-debian.sh
```

Installer tự chọn `linux/lbp2900-airprint-armhf.bin` trên Debian ARMHF. Nếu
không có binary phù hợp, nó mới fallback sang build native và khi đó cần
`build-essential`.

Muốn tự cross-build binary tĩnh trên máy khác bằng Zig:

```sh
make -C linux armhf-static ZIG=/duong/dan/toi/zig
```

Chương trình chạy root vì cần bind cổng 631 và claim USB interface. Backend sẽ
tạm detach `usblp` khi bắt đầu job rồi trả interface khi kết thúc.

Kiểm tra:

```sh
service lbp2900-airprint status
service avahi-daemon status
ss -ltn | grep -E ':631|:8080'
```

Mở `http://IP_CUA_BOX:8080/` hoặc thêm thủ công
`ipp://IP_CUA_BOX:631/ipp/print`.

## Chạy foreground để lấy log

```sh
service lbp2900-airprint stop
AIRPRINT_PORT=8631 AIRPRINT_WEB_PORT=8080 ./linux/lbp2900-airprint
```

Chỉ dùng trong LAN tin cậy. HTTP/IPP của dự án không có xác thực hoặc TLS.
