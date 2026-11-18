# Phân tích bản port Debian cho Canon LBP2900

## Kết luận

Repo gốc không thể cài trực tiếp lên Debian vì entry point, USB transport,
Wi-Fi và mDNS đều gắn với ESP-IDF. Tuy nhiên lõi IPP/AirPrint, bộ đọc URF/PWG
Raster và driver CAPT/Hi-SCoA là C thuần, nên có thể tái sử dụng. Bản port này
thay lớp phần cứng ESP32 bằng Linux USBFS và cung cấp daemon native ARMHF.

Mục tiêu đã chọn là Debian 11 ARMHF gốc của VNPT SmartBox 2 S805, kernel vendor
3.10.33 và SysV init. Không chuyển image sang systemd, không cần CUPS và không
cần libusb tại runtime.

## Thành phần được giữ lại

- `components/airprint`: HTTP/IPP server, trạng thái máy in và giải mã raster.
- `components/printers/canon_lbp2900.c`: chuỗi lệnh CAPT, chia trang thành band
  70 dòng và truyền dữ liệu nén Hi-SCoA.
- `components/canon_bjl`: bộ dựng/nén dữ liệu raster và self-test host.
- URF và PWG Raster là hai đầu vào chính từ client AirPrint.

## Thành phần Linux mới

- `linux/main.c`: entry point daemon, cấu hình qua biến môi trường và signal.
- `linux/usbfs_backend.c`: tìm USB `04a9:2676`, đọc descriptor, claim printer
  interface và thực hiện bulk OUT/IN hai chiều.
- `linux/lbp2900-airprint.init`: quản lý tiến trình bằng SysV
  `start-stop-daemon`.
- `linux/lbp2900-airprint.service`: quảng bá `_ipp._tcp` qua Avahi.
- `linux/install-debian.sh`: cài binary, init script, defaults và Avahi service.
- `linux/lbp2900-airprint-armhf.bin`: ELF ARM hard-float tĩnh, không phụ thuộc
  glibc/libusb/CUPS.

Luồng in là:

1. iPhone/macOS tìm dịch vụ bằng Avahi và gửi URF/PWG Raster qua IPP.
2. Lõi AirPrint giải mã từng dòng raster.
3. Driver LBP2900 gom 70 dòng, nén Hi-SCoA và tạo lệnh CAPT.
4. Backend USBFS gửi CAPT qua bulk OUT và nhận phản hồi qua bulk IN.
5. Khi job kết thúc, interface được release và `usblp` được reconnect nếu nó
   đã bị detach lúc bắt đầu.

## Sửa lỗi portability trong driver

- Delay 100 ms/1 giây trước đây chỉ tồn tại trên ESP32; Linux giờ dùng
  `nanosleep` thật.
- Phản hồi CAPT có thể ghi độ dài theo số nhị phân hoặc BCD. Driver và backend
  giờ chấp nhận cả hai kiểu, tương ứng với hành vi của captdriver Linux cũ.

## Mức tương thích với image S805

| Hạng mục | Kết quả |
|---|---|
| CPU Cortex-A5 / ARMv7 hard-float | Binary là ELF32 ARM EABI5 hard-float |
| Debian 11 ARMHF | Phù hợp ABI, binary liên kết tĩnh |
| Kernel 3.10.33 | Dùng USBFS ioctl lâu đời; vẫn cần kiểm tra kernel vendor có bật USB host/usbfs |
| SysV init | Có `/etc/init.d/lbp2900-airprint`; không dùng `systemctl` |
| CUPS | Không cần |
| libusb | Không cần |
| Avahi | Cần để client tự tìm thấy máy in; có thể nhập IPP URL thủ công nếu thiếu |
| Máy in | Chỉ Canon LBP2900/LBP2900B có USB ID `04a9:2676` |

## Giới hạn và rủi ro còn lại

- Driver CAPT vẫn là experimental và chưa được xác nhận bằng máy in thật trong
  môi trường build này. Không nên ghi image đã sửa vào thiết bị duy nhất trước
  khi có bản sao lưu.
- Chỉ quảng cáo đơn sắc 600 dpi, A4/Letter, không duplex.
- IPP không nhận PDF/JPEG trực tiếp; client phải gửi URF hoặc PWG Raster.
- HTTP/IPP không có TLS hay xác thực, chỉ nên dùng trong LAN tin cậy.
- Daemon cần quyền root để bind cổng 631 và claim USB. Đây là lựa chọn tương
  thích với image cũ, không phải mô hình hardening tối ưu.
- Image gốc nhỏ và gần đầy; nên mở rộng rootfs ít nhất 1 GiB trước khi cài thêm
  Avahi và chạy thử nhiều job.
- File driver LBP2900 dùng GPL-3.0-or-later. Binary liên kết driver này phải
  được phân phối theo GPL; gói đã kèm source và thông tin license.

## Checklist thử trên box thật

```sh
uname -a
dpkg --print-architecture
lsusb -d 04a9:2676
mount | grep usbfs || true
sudo sh linux/install-debian.sh
service lbp2900-airprint status
service avahi-daemon status
ss -ltn | grep -E ':631|:8080'
```

Trước tiên mở `http://IP_CUA_BOX:8080/`. Sau đó gửi một trang test A4 đơn giản,
theo dõi việc máy in nhận giấy, hoàn tất trang và daemon còn sống. Nếu lỗi, chạy
foreground để lấy stderr:

```sh
service lbp2900-airprint stop
AIRPRINT_PORT=8631 AIRPRINT_WEB_PORT=8080 /usr/local/sbin/lbp2900-airprint
```

Sau khi xác nhận USB ổn định, mới thử tài liệu nhiều trang, hủy job giữa chừng,
rút/cắm lại USB và reboot box.

## Gỡ cài đặt

```sh
service lbp2900-airprint stop || true
update-rc.d -f lbp2900-airprint remove
rm -f /etc/init.d/lbp2900-airprint
rm -f /etc/default/lbp2900-airprint
rm -f /etc/avahi/services/lbp2900-airprint.service
rm -f /usr/local/sbin/lbp2900-airprint
service avahi-daemon restart || true
```
