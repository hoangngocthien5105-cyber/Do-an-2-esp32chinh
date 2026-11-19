#ifndef SMARTKEY_H
#define SMARTKEY_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

// ================= 1. THÔNG SỐ GIAO TIẾP BLE (ĐỒNG BỘ VỚI ESP32-C3) =================
#define SMARTKEY_SERVICE_UUID           "12345678-1234-1234-1234-1234567890AB"
#define SMARTKEY_CHALLENGE_CHAR_UUID    "12345678-1234-1234-1234-1234567890AC"
#define SMARTKEY_RESPONSE_CHAR_UUID     "12345678-1234-1234-1234-1234567890AD"

#define SMARTKEY_CHALLENGE_LEN          16
#define SMARTKEY_RESPONSE_LEN           32
#define SMARTKEY_MAX_ID_LEN             16
#define SMARTKEY_MAX_KEY_LEN            32

// ================= 2. THÔNG SỐ BẢO MẬT & THỜI GIAN (NON-BLOCKING) =================
#define SMARTKEY_SCAN_DURATION_SEC      5       // Thời gian mỗi lượt quét BLE (giây)
#define SMARTKEY_RESCAN_INTERVAL_MS     3000    // Nghỉ giữa các lượt quét (ms)
#define SMARTKEY_CONNECT_TIMEOUT_SEC    5       // Timeout kết nối BLE (giây)
#define SMARTKEY_CHALLENGE_TIMEOUT_MS   5000    // Hạn sử dụng của mỗi Challenge (ms)
#define SMARTKEY_READ_DELAY_MS          250     // Chờ ESP32-C3 tính HMAC nếu dùng cơ chế READ (ms)
#define SMARTKEY_SESSION_DURATION_MS    30000   // Thời gian duy trì quyền truy cập sau xác thực (ms)
#define SMARTKEY_MAX_AUTH_FAILURES      3       // Số lần xác thực sai liên tiếp tối đa
#define SMARTKEY_LOCKOUT_DURATION_MS    30000   // Thời gian khóa tạm sau khi sai quá số lần (ms)

// ================= 3. CẤU TRÚC MỞ RỘNG NHIỀU SMARTKEY =================
struct SmartKeyEntry {
    const char *id;
    uint8_t secretKey[SMARTKEY_MAX_KEY_LEN];
    size_t secretKeyLen;
    bool enabled;
};

// ================= 4. TRẠNG THÁI & API CÔNG KHAI =================
extern volatile bool smartkeyAuthenticated;

// Khởi tạo BLE Central (gọi 1 lần trong setup())
void smartKeyInit();

// State machine non-blocking xử lý quét, kết nối, xác thực & hết hạn (gọi trong loop())
void smartKeyLoop();

// Kiểm tra SmartKey có đang hợp lệ (đã xác thực + còn kết nối + chưa hết hạn)
bool isSmartKeyAuthenticated();

// Lấy ID của SmartKey đang xác thực (ví dụ "SK001", rỗng "" nếu chưa xác thực)
const char *getAuthenticatedSmartKeyId();

// Thời gian quyền truy cập còn lại (ms), trả về 0 nếu không hợp lệ
unsigned long getSmartKeyRemainingMs();

// Thu hồi quyền truy cập ngay lập tức
void revokeSmartKeyAccess(bool logExpired = false);

// Kiểm tra chuỗi lệnh giọng nói có phải lệnh "Mở cửa" hay không
bool isOpenDoorVoiceCommand(const char *cmdText);

// Kiểm tra điều kiện kép (SmartKey hợp lệ + lệnh giọng nói "Mở cửa")
// Trả về true CHỈ KHI được phép mở cửa (không tự mở cửa khi mới xác thực SmartKey)
bool smartKeyAuthorizeOpenDoor(const char *voiceCmdText);

#endif // SMARTKEY_H
