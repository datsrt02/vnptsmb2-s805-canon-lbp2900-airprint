# Kết quả cài đặt và thử phần cứng ngày 2026-11-18

Thiết bị đích: VNPT SmartBox 2 S805 tại `192.168.1.33`.

## Đã xác nhận

- Debian 11.11 ARMHF, kernel vendor 3.10.33, SysV init.
- Canon LBP2900 được kernel nhận tại USB ID `04a9:2676`.
- `avahi-daemon` đã được cài và chạy sau khi thêm user `avahi` vào các nhóm
  mạng Android của image.
- `lbp2900-airprint` được bật khởi động cùng máy và lắng nghe cổng 631/8080.
- Web status nhận máy in và hiển thị `Ready`.
- Job URF A4 600 dpi thật đã đi qua IPP, giải mã raster, CAPT setup, truyền dữ
  liệu, CAPT FIRE và phản hồi USB hai chiều.
- Bộ đếm máy in đạt `recv=1`, `decode=1`.

## Trạng thái trang test

Máy in báo cờ CAPT `NOPAPER`; bộ đếm dừng ở `out=0`, `done=0`, nên chưa thể xác
nhận trang giấy vật lý. Sau khi nạp giấy A4/đóng đúng khay, gửi lại trang test để
xác nhận `out=1`, `done=1`.

Trong quá trình thử thật đã sửa thêm các lỗi chỉ xuất hiện trên phần cứng:

- Base status CAPT hợp lệ có thể chỉ dài 6 byte.
- `CAPT_START_0` phải đọc ACK trước `JOB_BEGIN`.
- Back-channel cần bỏ phản hồi cũ sau một job bị ngắt.
- Job có callback lỗi không được trả IPP success giả.
- Khi máy hết giấy và đã ngừng xử lý, daemon trả lỗi thay vì treo sáu phút.
- Sau khi Windows Test Page in thành công nhưng một tài liệu khác tạo job raw
  bị kẹt, capability IPP đã đổi sang PWG Raster/URF và daemon được bổ sung cơ
  chế thu hồi job processing không còn transport/thread.

## Thay đổi mạng trên box

`/etc/resolv.conf` trước đó trỏ tới DNS Tailscale `100.100.100.100` trong khi
Tailscale đang logged-out. Resolver đã được đổi sang router `192.168.1.1` và
`1.1.1.1`; bản cũ được giữ tại:

```text
/etc/resolv.conf.codex-backup-20261118
```
