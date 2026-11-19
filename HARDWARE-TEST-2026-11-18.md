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
- Windows Test Page và một trang PDF đã in ra giấy thành công.
- Bộ đếm máy in đạt đủ `recv=1`, `decode=1`, `out=1`, `done=1`.

## Kiểm tra lỗi job liên tiếp ngày 2026-11-19

Đã tái hiện có định hướng: job đầu hoàn tất với page ID `1`; job kế tiếp được
máy in cấp page ID `2`, nhưng driver cũ lại chờ cứng `received == 1`, dẫn tới
timeout rồi báo `printer-stopped`. Driver đã được sửa theo captdriver tham chiếu:
chờ `page_received == page_decoding`, sau đó dùng page ID do máy in cấp cho
`FIRE`, page-out và completion.

Sau khi cài binary mới, job đầu hoàn tất. Job thứ hai nhận đúng ID `2` và đi qua
điểm treo cũ, sau đó máy in trả cờ cảm biến thật `NOPAPER`
(`flags=8a12/0200`). Cần nạp/chỉnh lại giấy để hoàn tất phép thử hai trang liên
tiếp; lỗi bộ đếm CAPT đã được xác nhận không còn chặn job thứ hai.

Trong quá trình thử thật đã sửa thêm các lỗi chỉ xuất hiện trên phần cứng:

- Base status CAPT hợp lệ có thể chỉ dài 6 byte.
- `CAPT_START_0` phải đọc ACK trước `JOB_BEGIN`.
- Back-channel cần bỏ phản hồi cũ sau một job bị ngắt.
- Job có callback lỗi không được trả IPP success giả.
- Khi máy hết giấy và đã ngừng xử lý, daemon trả lỗi thay vì treo sáu phút.
- Sau khi Windows Test Page in thành công nhưng một tài liệu khác tạo job raw
  bị kẹt, capability IPP đã đổi sang PWG Raster/URF và daemon được bổ sung cơ
  chế thu hồi job processing không còn transport/thread.
- Page ID CAPT tiếp tục tăng giữa các IPP job; không được so sánh
  `page_received` với số trang cục bộ luôn bắt đầu từ 1.

## Thay đổi mạng trên box

`/etc/resolv.conf` trước đó trỏ tới DNS Tailscale `100.100.100.100` trong khi
Tailscale đang logged-out. Resolver đã được đổi sang router `192.168.1.1` và
`1.1.1.1`; bản cũ được giữ tại:

```text
/etc/resolv.conf.codex-backup-20261118
```
