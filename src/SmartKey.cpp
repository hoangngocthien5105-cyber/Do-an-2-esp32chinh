#include "SmartKey.h"

#include <NimBLEDevice.h>
#include <mbedtls/md.h>
#include <esp_system.h>
#include <string.h>
#include <ctype.h>

// ================= 1. DANH SÁCH SMARTKEY (SẴN SÀNG MỞ RỘNG NHIỀU THIẾT BỊ) =================
// LƯU Ý BẢO MẬT:
// - KHÔNG truyền Secret Key qua BLE.
// - KHÔNG in Secret Key ra Serial Monitor.
// - KHÔNG lưu Secret Key công khai trên Firebase.
// - Thay mảng secretKey bên dưới cho khớp 100% với Secret Key trong source ESP32-C3.
static const SmartKeyEntry SMARTKEY_REGISTRY[] = {
    {
        "SK001",
        {
            0x53, 0x4B, 0x30, 0x30, 0x31, 0x5F, 0x53, 0x45,
            0x43, 0x52, 0x45, 0x54, 0x5F, 0x4B, 0x45, 0x59,
            0x5F, 0x32, 0x30, 0x32, 0x36, 0x5F, 0x48, 0x4D,
            0x41, 0x43, 0x5F, 0x53, 0x48, 0x41, 0x32, 0x35
        },
        32,
        true
    }
};
static const size_t SMARTKEY_REGISTRY_COUNT =
    sizeof(SMARTKEY_REGISTRY) / sizeof(SMARTKEY_REGISTRY[0]);

// ================= 2. BIẾN TRẠNG THÁI NỘI BỘ (TỐI ƯU RAM, KHÔNG CẤP PHÁT ĐỘNG) =================
volatile bool smartkeyAuthenticated = false;

enum SmartKeyState : uint8_t {
    SK_STATE_IDLE = 0,
    SK_STATE_SCANNING,
    SK_STATE_DEVICE_FOUND,
    SK_STATE_WAIT_RESPONSE,
    SK_STATE_AUTHENTICATED
};

static SmartKeyState skState = SK_STATE_IDLE;
static portMUX_TYPE skMux = portMUX_INITIALIZER_UNLOCKED;

static const NimBLEUUID kServiceUUID(SMARTKEY_SERVICE_UUID);
static const NimBLEUUID kChallengeUUID(SMARTKEY_CHALLENGE_CHAR_UUID);
static const NimBLEUUID kResponseUUID(SMARTKEY_RESPONSE_CHAR_UUID);

// Tái sử dụng duy nhất 1 NimBLEClient để tránh phân mảnh Heap
static NimBLEClient *pClient = nullptr;
static NimBLERemoteCharacteristic *pChallengeChar = nullptr;
static NimBLERemoteCharacteristic *pResponseChar = nullptr;

static NimBLEAddress targetAddress;
static volatile bool targetFoundFlag = false;
static volatile bool scanFinishedFlag = false;
static volatile bool bleConnectedFlag = false;
static volatile bool disconnectEventFlag = false;

static bool responseUsesNotify = false;
static bool responseCanRead = false;

// Bộ đệm Challenge–Response (chỉ dùng 1 lần, xóa trắng sau khi xác minh)
static uint8_t currentChallenge[SMARTKEY_CHALLENGE_LEN] = {0};
static volatile bool challengeActive = false;
static unsigned long challengeSentMs = 0;

static uint8_t rxResponseBuf[SMARTKEY_RESPONSE_LEN] = {0};
static volatile size_t rxResponseLen = 0;
static volatile bool rxResponseReady = false;
static volatile bool rxResponseInvalidLen = false;

// Quản lý phiên truy cập & giới hạn lỗi
static char authenticatedId[SMARTKEY_MAX_ID_LEN] = {0};
static unsigned long authenticatedAtMs = 0;
static unsigned long lastScanEndMs = 0;
static uint8_t consecutiveFailures = 0;
static unsigned long lockoutUntilMs = 0;

// ================= 3. HÀM BẢO MẬT: HMAC-SHA256 & CONSTANT-TIME COMPARE =================
static bool computeHmacSha256(const uint8_t *key, size_t keyLen,
                              const uint8_t *data, size_t dataLen,
                              uint8_t outMac[SMARTKEY_RESPONSE_LEN]) {
    const mbedtls_md_info_t *mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mdInfo == nullptr) {
        return false;
    }
    int rc = mbedtls_md_hmac(mdInfo, key, keyLen, data, dataLen, outMac);
    return (rc == 0);
}

// So sánh thời gian hằng (constant-time) tránh timing side-channel
static bool constantTimeCompare(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (a[i] ^ b[i]);
    }
    return (diff == 0);
}

static void clearChallengeState() {
    portENTER_CRITICAL(&skMux);
    challengeActive = false;
    rxResponseReady = false;
    rxResponseInvalidLen = false;
    rxResponseLen = 0;
    memset(currentChallenge, 0, sizeof(currentChallenge));
    memset(rxResponseBuf, 0, sizeof(rxResponseBuf));
    portEXIT_CRITICAL(&skMux);
}

static void recordAuthFailure() {
    revokeSmartKeyAccess(false);
    clearChallengeState();
    Serial.println("[AUTH] XAC THUC THAT BAI!");

    consecutiveFailures++;
    if (consecutiveFailures >= SMARTKEY_MAX_AUTH_FAILURES) {
        lockoutUntilMs = millis() + SMARTKEY_LOCKOUT_DURATION_MS;
        consecutiveFailures = 0;
        Serial.printf("[AUTH] Sai qua %d lan, tam khoa xac thuc trong %lu giay!\n",
                      SMARTKEY_MAX_AUTH_FAILURES,
                      (unsigned long)(SMARTKEY_LOCKOUT_DURATION_MS / 1000UL));
    }
}

static void safeDisconnect() {
    pChallengeChar = nullptr;
    pResponseChar = nullptr;
    if (pClient != nullptr && pClient->isConnected()) {
        pClient->disconnect();
    }
    bleConnectedFlag = false;
}

// ================= 4. CALLBACKS CỦA NIMBLE (CHẠY TRÊN NIMBLE HOST TASK) =================
class SmartKeyClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient *client) override {
        (void)client;
        bleConnectedFlag = true;
        disconnectEventFlag = false;
    }

    void onDisconnect(NimBLEClient *client) override {
        (void)client;
        bleConnectedFlag = false;
        disconnectEventFlag = true;
        smartkeyAuthenticated = false;
    }
};

class SmartKeyAdvertisedCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *advertisedDevice) override {
        if (targetFoundFlag) return;
        // Chỉ lọc theo Service UUID đã thống nhất (không dùng tên/MAC/RSSI để xác thực)
        if (advertisedDevice->isAdvertisingService(kServiceUUID)) {
            targetAddress = advertisedDevice->getAddress();
            targetFoundFlag = true;
            NimBLEDevice::getScan()->stop();
        }
    }
};

static SmartKeyClientCallbacks clientCallbacks;
static SmartKeyAdvertisedCallbacks advertisedCallbacks;

static void scanCompleteCallback(NimBLEScanResults results) {
    (void)results;
    scanFinishedFlag = true;
}

static void responseNotifyCallback(NimBLERemoteCharacteristic *pChar,
                                   uint8_t *pData,
                                   size_t length,
                                   bool isNotify) {
    (void)pChar;
    (void)isNotify;

    portENTER_CRITICAL(&skMux);
    // Bỏ qua Response cũ hoặc đến khi không có Challenge hợp lệ
    if (!challengeActive || rxResponseReady) {
        portEXIT_CRITICAL(&skMux);
        return;
    }
    if (length == SMARTKEY_RESPONSE_LEN && pData != nullptr) {
        memcpy(rxResponseBuf, pData, SMARTKEY_RESPONSE_LEN);
        rxResponseLen = length;
        rxResponseInvalidLen = false;
        rxResponseReady = true;
    } else {
        rxResponseLen = length;
        rxResponseInvalidLen = true;
        rxResponseReady = true;
    }
    portEXIT_CRITICAL(&skMux);
}

// ================= 5. QUY TRÌNH QUÉT, KẾT NỐI & XÁC THỰC =================
static void startBleScan() {
    NimBLEScan *pScan = NimBLEDevice::getScan();
    if (pScan == nullptr) return;

    pScan->clearResults();
    targetFoundFlag = false;
    scanFinishedFlag = false;

    Serial.println("[BLE] Dang tim kiem SmartKey...");
    bool started = pScan->start(SMARTKEY_SCAN_DURATION_SEC, scanCompleteCallback, false);
    if (started) {
        skState = SK_STATE_SCANNING;
    } else {
        lastScanEndMs = millis();
        skState = SK_STATE_IDLE;
    }
}

static bool connectAndStartAuth() {
    if (pClient == nullptr) {
        pClient = NimBLEDevice::createClient();
        if (pClient == nullptr) {
            Serial.println("[SMARTKEY] Loi tao BLE Client!");
            return false;
        }
        pClient->setClientCallbacks(&clientCallbacks, false);
        pClient->setConnectTimeout(SMARTKEY_CONNECT_TIMEOUT_SEC);
        pClient->setConnectionParams(12, 24, 0, 100);
    }

    if (pClient->isConnected()) {
        pClient->disconnect();
    }

    disconnectEventFlag = false;
    if (!pClient->connect(targetAddress)) {
        Serial.println("[SMARTKEY] Ket noi that bai!");
        return false;
    }

    bleConnectedFlag = true;
    Serial.println("[SMARTKEY] Ket noi thanh cong!");

    // Bước 2: Tìm đúng Service UUID & 2 Characteristic Challenge / Response
    NimBLERemoteService *pService = pClient->getService(kServiceUUID);
    if (pService == nullptr) {
        Serial.println("[AUTH] Loi: Khong tim thay SmartKey Service UUID!");
        safeDisconnect();
        return false;
    }

    pChallengeChar = pService->getCharacteristic(kChallengeUUID);
    pResponseChar  = pService->getCharacteristic(kResponseUUID);
    if (pChallengeChar == nullptr || pResponseChar == nullptr) {
        Serial.println("[AUTH] Loi: Khong tim thay Characteristic Challenge/Response!");
        safeDisconnect();
        return false;
    }

    bool canWriteWithResp = pChallengeChar->canWrite();
    bool canWriteNoResp   = pChallengeChar->canWriteNoResponse();
    if (!canWriteWithResp && !canWriteNoResp) {
        Serial.println("[AUTH] Loi: Challenge Characteristic khong ho tro WRITE!");
        safeDisconnect();
        return false;
    }

    bool canNotify   = pResponseChar->canNotify();
    bool canIndicate = pResponseChar->canIndicate();
    responseCanRead  = pResponseChar->canRead();
    if (!canNotify && !canIndicate && !responseCanRead) {
        Serial.println("[AUTH] Loi: Response Characteristic khong ho tro NOTIFY/READ!");
        safeDisconnect();
        return false;
    }

    // Đăng ký nhận Notify/Indicate TRƯỚC khi gửi Challenge để không bỏ lỡ gói tin
    responseUsesNotify = false;
    if (canNotify || canIndicate) {
        if (pResponseChar->subscribe(canNotify, responseNotifyCallback)) {
            responseUsesNotify = true;
        } else if (!responseCanRead) {
            Serial.println("[AUTH] Loi: Khong dang ky duoc Notify cho Response!");
            safeDisconnect();
            return false;
        }
    }

    // Bước 3: Tạo Challenge ngẫu nhiên 16 byte bằng Hardware TRNG của ESP32
    Serial.println("[AUTH] Dang xac thuc SmartKey...");
    clearChallengeState();

    uint8_t freshChallenge[SMARTKEY_CHALLENGE_LEN];
    esp_fill_random(freshChallenge, SMARTKEY_CHALLENGE_LEN);

    portENTER_CRITICAL(&skMux);
    memcpy(currentChallenge, freshChallenge, SMARTKEY_CHALLENGE_LEN);
    challengeActive = true;
    rxResponseReady = false;
    rxResponseInvalidLen = false;
    rxResponseLen = 0;
    portEXIT_CRITICAL(&skMux);
    memset(freshChallenge, 0, sizeof(freshChallenge));

    // Bước 4: Gửi đúng 16 byte nhị phân thô qua Challenge Characteristic
    bool writeOk = pChallengeChar->writeValue(currentChallenge,
                                              SMARTKEY_CHALLENGE_LEN,
                                              canWriteWithResp);
    if (!writeOk) {
        Serial.println("[AUTH] Loi: Khong gui duoc Challenge!");
        clearChallengeState();
        safeDisconnect();
        return false;
    }

    challengeSentMs = millis();
    Serial.println("[AUTH] Da gui Challenge!");
    skState = SK_STATE_WAIT_RESPONSE;
    return true;
}

static void verifyReceivedResponse(const uint8_t *received, size_t len) {
    Serial.println("[AUTH] Da nhan Response!");

    // Thu hồi ngay Challenge để đảm bảo mỗi Challenge chỉ dùng 1 lần
    uint8_t challengeCopy[SMARTKEY_CHALLENGE_LEN];
    portENTER_CRITICAL(&skMux);
    memcpy(challengeCopy, currentChallenge, SMARTKEY_CHALLENGE_LEN);
    challengeActive = false;
    memset(currentChallenge, 0, sizeof(currentChallenge));
    rxResponseReady = false;
    portEXIT_CRITICAL(&skMux);

    if (len != SMARTKEY_RESPONSE_LEN) {
        memset(challengeCopy, 0, sizeof(challengeCopy));
        recordAuthFailure();
        safeDisconnect();
        lastScanEndMs = millis();
        skState = SK_STATE_IDLE;
        return;
    }

    // Bước 6: Tính HMAC-SHA256 và so sánh constant-time với danh sách SmartKey hợp lệ
    const SmartKeyEntry *matchedEntry = nullptr;
    uint8_t expectedMac[SMARTKEY_RESPONSE_LEN];

    for (size_t i = 0; i < SMARTKEY_REGISTRY_COUNT; i++) {
        if (!SMARTKEY_REGISTRY[i].enabled) continue;

        if (computeHmacSha256(SMARTKEY_REGISTRY[i].secretKey,
                              SMARTKEY_REGISTRY[i].secretKeyLen,
                              challengeCopy,
                              SMARTKEY_CHALLENGE_LEN,
                              expectedMac)) {
            if (constantTimeCompare(expectedMac, received, SMARTKEY_RESPONSE_LEN)) {
                matchedEntry = &SMARTKEY_REGISTRY[i];
                break;
            }
        }
    }

    // Xóa bộ đệm nhạy cảm trên stack ngay sau khi so sánh
    memset(challengeCopy, 0, sizeof(challengeCopy));
    memset(expectedMac, 0, sizeof(expectedMac));

    if (matchedEntry != nullptr) {
        // Bước 7: Cấp quyền truy cập (KHÔNG tự động mở cửa)
        portENTER_CRITICAL(&skMux);
        strncpy(authenticatedId, matchedEntry->id, SMARTKEY_MAX_ID_LEN - 1);
        authenticatedId[SMARTKEY_MAX_ID_LEN - 1] = '\0';
        authenticatedAtMs = millis();
        smartkeyAuthenticated = true;
        portEXIT_CRITICAL(&skMux);

        consecutiveFailures = 0;
        Serial.println("[AUTH] XAC THUC THANH CONG!");
        Serial.printf("[AUTH] SmartKey ID: %s\n", authenticatedId);
        skState = SK_STATE_AUTHENTICATED;
    } else {
        recordAuthFailure();
        safeDisconnect();
        lastScanEndMs = millis();
        skState = SK_STATE_IDLE;
    }
}

// ================= 6. API CÔNG KHAI =================
void smartKeyInit() {
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P3); // Mức công suất vừa phải, ổn định Wi-Fi + BLE

    NimBLEScan *pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(&advertisedCallbacks, false);
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(45); // Window < Interval giúp Wi-Fi & Audio UDP chạy mượt khi quét BLE
    pScan->setMaxResults(0); // Không lưu danh sách thiết bị dư thừa trong RAM

    smartkeyAuthenticated = false;
    authenticatedId[0] = '\0';
    lastScanEndMs = 0;
    skState = SK_STATE_IDLE;

    Serial.println("[BLE] BLE Central da khoi dong!");
}

void smartKeyLoop() {
    unsigned long now = millis();

    // Xử lý sự kiện mất kết nối BLE bất đồng bộ
    if (disconnectEventFlag) {
        disconnectEventFlag = false;
        bool wasActive = (skState == SK_STATE_WAIT_RESPONSE || skState == SK_STATE_AUTHENTICATED);
        if (wasActive) {
            revokeSmartKeyAccess(false);
            clearChallengeState();
            pChallengeChar = nullptr;
            pResponseChar = nullptr;
            Serial.println("[SMARTKEY] Mat ket noi!");
            lastScanEndMs = now;
            skState = SK_STATE_IDLE;
        }
    }

    switch (skState) {
        case SK_STATE_IDLE: {
            if (lockoutUntilMs != 0 && (long)(now - lockoutUntilMs) < 0) {
                return; // Đang trong thời gian khóa tạm do xác thực sai quá số lần
            }
            lockoutUntilMs = 0;

            if (lastScanEndMs == 0 || (now - lastScanEndMs >= SMARTKEY_RESCAN_INTERVAL_MS)) {
                startBleScan();
            }
            break;
        }

        case SK_STATE_SCANNING: {
            if (targetFoundFlag) {
                Serial.println("[SMARTKEY] Da phat hien thiet bi!");
                skState = SK_STATE_DEVICE_FOUND;
            } else if (scanFinishedFlag) {
                scanFinishedFlag = false;
                lastScanEndMs = now;
                skState = SK_STATE_IDLE;
            }
            break;
        }

        case SK_STATE_DEVICE_FOUND: {
            targetFoundFlag = false;
            if (!connectAndStartAuth()) {
                lastScanEndMs = now;
                skState = SK_STATE_IDLE;
            }
            break;
        }

        case SK_STATE_WAIT_RESPONSE: {
            if (!bleConnectedFlag || pClient == nullptr || !pClient->isConnected()) {
                revokeSmartKeyAccess(false);
                clearChallengeState();
                Serial.println("[SMARTKEY] Mat ket noi!");
                safeDisconnect();
                lastScanEndMs = now;
                skState = SK_STATE_IDLE;
                break;
            }

            // Kiểm tra quá hạn Challenge
            if (now - challengeSentMs >= SMARTKEY_CHALLENGE_TIMEOUT_MS) {
                recordAuthFailure();
                safeDisconnect();
                lastScanEndMs = now;
                skState = SK_STATE_IDLE;
                break;
            }

            // Kiểm tra đã nhận Response qua Notify chưa
            bool ready = false;
            bool badLen = false;
            size_t len = 0;
            uint8_t localResp[SMARTKEY_RESPONSE_LEN];

            portENTER_CRITICAL(&skMux);
            if (rxResponseReady) {
                ready = true;
                badLen = rxResponseInvalidLen;
                len = rxResponseLen;
                if (!badLen && len == SMARTKEY_RESPONSE_LEN) {
                    memcpy(localResp, rxResponseBuf, SMARTKEY_RESPONSE_LEN);
                }
                memset(rxResponseBuf, 0, sizeof(rxResponseBuf));
                rxResponseReady = false;
            }
            portEXIT_CRITICAL(&skMux);

            if (ready) {
                verifyReceivedResponse(localResp, badLen ? 0 : len);
                memset(localResp, 0, sizeof(localResp));
                break;
            }

            // Trường hợp ESP32-C3 chỉ dùng READ (không dùng Notify)
            if (!responseUsesNotify && responseCanRead &&
                (now - challengeSentMs >= SMARTKEY_READ_DELAY_MS) &&
                pResponseChar != nullptr) {
                std::string val = pResponseChar->readValue();
                if (val.size() == SMARTKEY_RESPONSE_LEN) {
                    verifyReceivedResponse(reinterpret_cast<const uint8_t *>(val.data()), val.size());
                } else {
                    verifyReceivedResponse(nullptr, val.size());
                }
            }
            break;
        }

        case SK_STATE_AUTHENTICATED: {
            if (!bleConnectedFlag || pClient == nullptr || !pClient->isConnected()) {
                revokeSmartKeyAccess(false);
                Serial.println("[SMARTKEY] Mat ket noi!");
                safeDisconnect();
                lastScanEndMs = now;
                skState = SK_STATE_IDLE;
                break;
            }

            // Kiểm tra hết thời hạn quyền truy cập
            if (now - authenticatedAtMs >= SMARTKEY_SESSION_DURATION_MS) {
                revokeSmartKeyAccess(true);
                safeDisconnect();
                lastScanEndMs = now;
                skState = SK_STATE_IDLE;
            }
            break;
        }
    }
}

void revokeSmartKeyAccess(bool logExpired) {
    bool wasAuth = smartkeyAuthenticated;
    portENTER_CRITICAL(&skMux);
    smartkeyAuthenticated = false;
    authenticatedId[0] = '\0';
    authenticatedAtMs = 0;
    portEXIT_CRITICAL(&skMux);

    if (logExpired && wasAuth) {
        Serial.println("[AUTH] Quyen truy cap da het han!");
    }
}

bool isSmartKeyAuthenticated() {
    if (!smartkeyAuthenticated || !bleConnectedFlag) {
        return false;
    }
    if (millis() - authenticatedAtMs >= SMARTKEY_SESSION_DURATION_MS) {
        revokeSmartKeyAccess(true);
        return false;
    }
    return true;
}

const char *getAuthenticatedSmartKeyId() {
    return isSmartKeyAuthenticated() ? authenticatedId : "";
}

unsigned long getSmartKeyRemainingMs() {
    if (!isSmartKeyAuthenticated()) return 0;
    unsigned long elapsed = millis() - authenticatedAtMs;
    return (elapsed < SMARTKEY_SESSION_DURATION_MS) ? (SMARTKEY_SESSION_DURATION_MS - elapsed) : 0;
}

// ================= 7. KIỂM TRA LỆNH GIỌNG NÓI "MỞ CỬA" =================
bool isOpenDoorVoiceCommand(const char *cmdText) {
    if (cmdText == nullptr || cmdText[0] == '\0') return false;

    String s(cmdText);
    s.toLowerCase();

    return (s.indexOf("mo cua") >= 0 ||
            s.indexOf("mở cửa") >= 0 ||
            s.indexOf("door:open") >= 0);
}

bool smartKeyAuthorizeOpenDoor(const char *voiceCmdText) {
    if (!isOpenDoorVoiceCommand(voiceCmdText)) {
        return false;
    }

    if (isSmartKeyAuthenticated()) {
        Serial.printf("[DOOR] SmartKey hop le (%s) + Lenh giong noi hop le -> CHO PHEP MO CUA!\n",
                      getAuthenticatedSmartKeyId());
        return true;
    }

    Serial.println("[DOOR] Tu choi mo cua: Khong co SmartKey hop le hoac quyen da het han!");
    return false;
}
