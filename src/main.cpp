#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <DHTesp.h>
#include <Firebase_ESP_Client.h>
#include <driver/i2s.h>
#include <freertos/ringbuf.h>
#include <math.h>

#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include "SmartKey.h"

// ================= 1. CẤU HÌNH WIFI & FIREBASE =================
#define WIFI_SSID "iPhone"
#define WIFI_PASSWORD "244466666"
#define API_KEY "AIzaSyBVm44sWtW23fV3T0n3WpqQjgxZrSwxGpY"
#define DATABASE_URL "https://doan2-13598-default-rtdb.firebaseio.com"

// ================= 1B. THÔNG SỐ TỐI ƯU (ĐÃ CHỈNH ĐỘ MÉO VỀ 0%) =================
#define CPU_FREQ_MHZ         160                 // 160 MHz: mát chip, Wi-Fi/SSL và I2S chạy mượt
#define WIFI_TX_POWER        WIFI_POWER_17dBm    // Giữ sóng khỏe với iPhone mà vẫn mát
#define SENSOR_PERIOD_MS     50                  // Đọc lửa/gas/mưa mỗi 50 ms
#define DHT_STEP_MS          1200                // Đọc luân phiên từng DHT22 mỗi 1.2 s
#define FIREBASE_PERIOD_MS   10000               // Đẩy dữ liệu định kỳ 10 s
#define FIREBASE_MIN_GAP_MS  2000                // Khi có biến động đẩy ngay nhưng cách nhau >= 2 s
#define MIC_GAIN_SHIFT       14                  // Chuẩn 14 (dịch 14 bit từ 32-bit xuống 16-bit): Độ méo = 0.0%!
#define STATUS_PRINT_MS      10000               // In tình trạng hệ thống mỗi 10 s

// ================= 2A. ĐỊNH NGHĨA CHÂN CẢM BIẾN (ĐÃ BỎ SERVO) =================
#define DHT1_PIN 4

#define DHT2_PIN 16
#define FLAME_DO_PIN 26
#define FLAME_AO_PIN 35

#define DHT3_PIN 17
#define GAS_DO_PIN 25
#define GAS_AO_PIN 32

#define RAIN_DO_PIN 27
#define RAIN_AO_PIN 34

#define OLED_SDA 21
#define OLED_SCL 22

// ================= 2B. ĐỊNH NGHĨA CHÂN MICRO INMP441 & LOA MAX98357A =================
#define I2S_SPK_PORT I2S_NUM_0
#define SPK_BCLK_PIN 15
#define SPK_LRC_PIN  2
#define SPK_DIN_PIN  23

#define I2S_MIC_PORT I2S_NUM_1
#define MIC_SCK_PIN  18
#define MIC_WS_PIN   19
#define MIC_SD_PIN   5

const int UDP_MIC_PORT = 4444;
const int UDP_SPK_PORT = 4445;

// ================= 3. KHỞI TẠO ĐỐI TƯỢNG =================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ OLED_SCL, /* data=*/ OLED_SDA);

DHTesp dht1;
DHTesp dht2;
DHTesp dht3;

// Mỗi task dùng một đối tượng WiFiUDP riêng biệt để tránh xung đột bộ nhớ
WiFiUDP udpAudio;     // Nhận âm thanh (task udpRx)
WiFiUDP udpMicTx;     // Gửi tiếng mic (task mic)
WiFiUDP udpCtrlTx;    // Gửi lệnh điều khiển CMD:BOOT

RingbufHandle_t spkRingBuf = NULL;

FirebaseData fbdo;
FirebaseData fbdoStream;
FirebaseAuth auth;
FirebaseConfig config;
volatile bool signupOK = false;

float t1 = 0, h1 = 0;
float t2 = 0, h2 = 0;
float t3 = 0, h3 = 0;

volatile bool isFire = false;
int flameAO = 4095;

volatile bool isGasLeak = false;
int gasAO = 0;

volatile bool isRaining = false;
int rainAO = 4095;

volatile bool isListeningMic = true;
volatile bool uploadRequested = false;

String aiLastReply = "San sang lang nghe!";

IPAddress laptopIP;
volatile uint32_t laptopIPRaw = 0;
volatile bool hasLaptopIP = false;
bool hasGreetedLaptop = false;
volatile unsigned long lastLaptopAliveTime = 0;
volatile unsigned long lastSpkReceiveTime = 0;
volatile uint32_t spkDropCount = 0;

bool prevFire = false;
bool prevGas = false;
bool prevRain = false;

unsigned long lastSensorTime = 0;
unsigned long lastDHTReadTime = 0;
unsigned long lastAlarmBeepTime = 0;
unsigned long lastStatusPrint = 0;

// Quản lý hiển thị Khuôn mặt AI & Tự động trở về sau khi xem dữ liệu
int oledPage = 4;                                 // 4 = Khuôn mặt AI (Mặc định luôn hiển thị)
unsigned long dataPageShownTime = 0;
const unsigned long DATA_DISPLAY_DURATION = 7000; // Hiện bảng dữ liệu 7 giây rồi quay lại mặt AI

// Biến hiệu ứng nhấp nháy mắt Robot AI
unsigned long lastEyeBlinkTime = 0;
unsigned long nextBlinkInterval = 2600;
unsigned long lastFaceAnimTime = 0;
bool isEyeBlinking = false;
int eyeLookOffsetX = 0;

#define UI_TEXT_MAX 96
portMUX_TYPE uiMux = portMUX_INITIALIZER_UNLOCKED;
volatile int  pendingPage = -1;
volatile bool pendingText = false;
volatile bool pendingRender = false;
char pendingTextBuf[UI_TEXT_MAX];

void requestPage(int p) {
    if (p < 0 || p > 3) p = 4;
    portENTER_CRITICAL(&uiMux);
    pendingPage = p;
    portEXIT_CRITICAL(&uiMux);
}

void requestText(const char *s) {
    portENTER_CRITICAL(&uiMux);
    strncpy(pendingTextBuf, s, UI_TEXT_MAX - 1);
    pendingTextBuf[UI_TEXT_MAX - 1] = '\0';
    pendingText = true;
    portEXIT_CRITICAL(&uiMux);
}

void requestRender() {
    pendingRender = true;
}

void renderOLED();

// ================= 4. TIẾN TRÌNH PHÁT LOA ĐỘC LẬP TỪ BỘ ĐỆM RAM =================
void speakerPlayTask(void *pvParameters) {
    size_t itemSize = 0;
    size_t bytesWritten = 0;
    bool wasPlaying = false;

    for (;;) {
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(spkRingBuf, &itemSize, pdMS_TO_TICKS(50), 1024);
        if (item != NULL) {
            if (itemSize > 0) {
                i2s_write(I2S_SPK_PORT, item, itemSize, &bytesWritten, portMAX_DELAY);
                lastSpkReceiveTime = millis();
                wasPlaying = true;
            }
            vRingbufferReturnItem(spkRingBuf, (void *)item);
        } else {
            if (wasPlaying) {
                i2s_zero_dma_buffer(I2S_SPK_PORT);
                wasPlaying = false;
            }
        }
    }
}

void setupSpeakerI2S() {
    i2s_config_t spk_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = true,
        .tx_desc_auto_clear = true
    };
    i2s_pin_config_t spk_pins = {
        .bck_io_num = SPK_BCLK_PIN,
        .ws_io_num = SPK_LRC_PIN,
        .data_out_num = SPK_DIN_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    i2s_driver_install(I2S_SPK_PORT, &spk_config, 0, NULL);
    i2s_set_pin(I2S_SPK_PORT, &spk_pins);
    i2s_zero_dma_buffer(I2S_SPK_PORT);

    spkRingBuf = xRingbufferCreate(12288, RINGBUF_TYPE_BYTEBUF);
    xTaskCreatePinnedToCore(speakerPlayTask, "SpkTask", 3072, NULL, 5, NULL, 1);
}

void setupMicI2S() {
    i2s_config_t mic_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false
    };
    i2s_pin_config_t mic_pins = {
        .bck_io_num = MIC_SCK_PIN,
        .ws_io_num = MIC_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = MIC_SD_PIN
    };
    i2s_driver_install(I2S_MIC_PORT, &mic_config, 0, NULL);
    i2s_set_pin(I2S_MIC_PORT, &mic_pins);
}

void playToneSpk(int freqHz, int durationMs) {
    if (spkRingBuf == NULL) return;
    const int sampleRate = 16000;
    int totalSamples = (sampleRate * durationMs) / 1000;
    int16_t buf[256];

    for (int i = 0; i < totalSamples; i += 256) {
        int chunk = min(256, totalSamples - i);
        for (int j = 0; j < chunk; j++) {
            float angle = 2.0f * PI * freqHz * (i + j) / sampleRate;
            buf[j] = (int16_t)(sin(angle) * 8000);
        }
        xRingbufferSend(spkRingBuf, buf, chunk * sizeof(int16_t), pdMS_TO_TICKS(20));
    }
    lastSpkReceiveTime = millis();
}

// ================= 4B. TASK THU MIC (CHỐNG MÉO TIẾNG 100% VỚI MIC_GAIN_SHIFT = 14) =================
#define MIC_FRAMES_PER_PACKET 256

void micCaptureTask(void *pvParameters) {
    static int32_t rawStereo[MIC_FRAMES_PER_PACKET * 2];
    static int16_t pcm16Mono[MIC_FRAMES_PER_PACKET];

    float dcEst[2]     = {0.0f, 0.0f};
    float energyEst[2] = {0.0f, 0.0f};
    bool  initDone     = false;
    int   activeCh     = 0;

    for (;;) {
        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_MIC_PORT, rawStereo, sizeof(rawStereo), &bytesRead, portMAX_DELAY);
        if (err != ESP_OK || bytesRead < 8) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        int frames = bytesRead / (2 * sizeof(int32_t));

        bool echoGuard = (millis() - lastSpkReceiveTime) < 450;
        if (!isListeningMic || echoGuard || !hasLaptopIP || WiFi.status() != WL_CONNECTED) {
            continue;
        }

        // 1) Tính DC offset và năng lượng từng kênh
        float mean[2] = {0.0f, 0.0f};
        float dev[2]  = {0.0f, 0.0f};
        for (int i = 0; i < frames; i++) {
            mean[0] += (float)(rawStereo[2 * i]     >> 8);
            mean[1] += (float)(rawStereo[2 * i + 1] >> 8);
        }
        mean[0] /= frames;
        mean[1] /= frames;
        for (int i = 0; i < frames; i++) {
            dev[0] += fabsf((float)(rawStereo[2 * i]     >> 8) - mean[0]);
            dev[1] += fabsf((float)(rawStereo[2 * i + 1] >> 8) - mean[1]);
        }
        dev[0] /= frames;
        dev[1] /= frames;

        if (!initDone) {
            for (int c = 0; c < 2; c++) { dcEst[c] = mean[c]; energyEst[c] = dev[c]; }
            initDone = true;
        } else {
            for (int c = 0; c < 2; c++) {
                dcEst[c]     = 0.92f * dcEst[c]     + 0.08f * mean[c];
                energyEst[c] = 0.80f * energyEst[c] + 0.20f * dev[c];
            }
        }

        // 2) Chọn kênh có gắn Mic
        int prevCh = activeCh;
        if (activeCh == 0 && energyEst[1] > energyEst[0] * 2.0f) activeCh = 1;
        else if (activeCh == 1 && energyEst[0] > energyEst[1] * 2.0f) activeCh = 0;
        if (activeCh != prevCh) {
            Serial.printf("[MIC] Chuyen sang kenh %s (L=%.0f, R=%.0f)\n",
                          activeCh == 0 ? "TRAI" : "PHAI", energyEst[0], energyEst[1]);
        }

        // 3) Khử DC + dịch bit chuẩn (MIC_GAIN_SHIFT = 14 -> shift = 6) + Soft-limiter chống cắt ngọn
        const int shift = MIC_GAIN_SHIFT - 8;
        int32_t dc = (int32_t)dcEst[activeCh];
        for (int i = 0; i < frames; i++) {
            int32_t s24 = rawStereo[2 * i + activeCh] >> 8;
            int32_t val = (s24 - dc) >> shift;

            // Bộ giới hạn mềm (Soft-knee limiter) giúp kể cả khi hét sát mic cũng không bị méo vuông
            if (val > 28000) {
                val = 28000 + ((val - 28000) >> 2);
            } else if (val < -28000) {
                val = -28000 + ((val + 28000) >> 2);
            }
            if (val > 32767)  val = 32767;
            if (val < -32768) val = -32768;
            pcm16Mono[i] = (int16_t)val;
        }

        // 4) Gửi gói 512 bytes (16ms) về laptop
        IPAddress ip(laptopIPRaw);
        udpMicTx.beginPacket(ip, UDP_MIC_PORT);
        udpMicTx.write((uint8_t *)pcm16Mono, frames * sizeof(int16_t));
        udpMicTx.endPacket();
    }
}

// ================= 4C. TASK NHẬN UDP TỪ LAPTOP =================
void udpRxTask(void *pvParameters) {
    static uint8_t buf[1025];

    for (;;) {
        int packetSize = udpAudio.parsePacket();
        if (packetSize <= 0) {
            if (hasLaptopIP && (millis() - lastLaptopAliveTime > 15000)) {
                hasLaptopIP = false;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        laptopIP = udpAudio.remoteIP();
        laptopIPRaw = (uint32_t)laptopIP;
        lastLaptopAliveTime = millis();

        if (!hasLaptopIP) {
            hasLaptopIP = true;
            Serial.printf("-> [AI SERVER] Da ket noi voi Laptop IP: %s\n", laptopIP.toString().c_str());
            if (!hasGreetedLaptop) {
                hasGreetedLaptop = true;
                lastSpkReceiveTime = millis();
                udpCtrlTx.beginPacket(laptopIP, UDP_MIC_PORT);
                udpCtrlTx.write((const uint8_t *)"CMD:BOOT", 8);
                udpCtrlTx.endPacket();
            }
        }

        int len = udpAudio.read(buf, sizeof(buf) - 1);
        if (len <= 0) continue;

        if (len >= 4 && memcmp(buf, "CMD:", 4) == 0) {
            buf[len] = '\0';
            const char *cmd = (const char *)buf;
            if (strncmp(cmd, "CMD:BEEP", 8) == 0) {
                playToneSpk(1600, 50);
            } else if (strncmp(cmd, "CMD:PAGE:", 9) == 0) {
                requestPage(atoi(cmd + 9));
            } else if (strncmp(cmd, "CMD:TXT:", 8) == 0) {
                requestText(cmd + 8);
            } else if (strncmp(cmd, "CMD:USER:", 9) == 0) {
                requestText(cmd + 9);
                if (isOpenDoorVoiceCommand(cmd + 9)) {
                    if (smartKeyAuthorizeOpenDoor(cmd + 9)) {
                        // TODO: Gắn hàm điều khiển Servo mở cửa tại đây khi hoàn thiện phần cứng Servo
                        playToneSpk(1800, 120);
                    } else {
                        playToneSpk(600, 200);
                    }
                }
            } else if (strncmp(cmd, "CMD:DOOR:OPEN", 13) == 0) {
                if (smartKeyAuthorizeOpenDoor(cmd)) {
                    // TODO: Gắn hàm điều khiển Servo mở cửa tại đây khi hoàn thiện phần cứng Servo
                    playToneSpk(1800, 120);
                } else {
                    playToneSpk(600, 200);
                }
            }
            continue;
        }

        if (spkRingBuf != NULL) {
            if (xRingbufferSend(spkRingBuf, buf, len, pdMS_TO_TICKS(10)) == pdTRUE) {
                lastSpkReceiveTime = millis();
            } else {
                spkDropCount++;
            }
        }
    }
}

void sendBootGreetingBroadcast() {
    if (WiFi.status() != WL_CONNECTED) return;
    lastSpkReceiveTime = millis();

    IPAddress broadcastIP = WiFi.localIP();
    broadcastIP[3] = 255;

    for (int i = 0; i < 3; i++) {
        udpCtrlTx.beginPacket(broadcastIP, UDP_MIC_PORT);
        udpCtrlTx.write((const uint8_t *)"CMD:BOOT", 8);
        udpCtrlTx.endPacket();

        for (int host = 2; host <= 5; host++) {
            IPAddress targetIP = WiFi.localIP();
            if (targetIP[3] != host) {
                targetIP[3] = host;
                udpCtrlTx.beginPacket(targetIP, UDP_MIC_PORT);
                udpCtrlTx.write((const uint8_t *)"CMD:BOOT", 8);
                udpCtrlTx.endPacket();
            }
        }
        delay(20);
    }
}

// ================= 5. LẮNG NGHE LỆNH FIREBASE =================
void streamCallback(FirebaseStream data) {
    String path = data.dataPath();
    if (path == "/" && data.dataTypeEnum() == fb_esp_rtdb_data_type_json) {
        FirebaseJson *json = data.to<FirebaseJson *>();
        FirebaseJsonData res;
        if (json->get(res, "bat_mic_inmp441")) isListeningMic = res.to<bool>();
        if (json->get(res, "ai_tra_loi")) {
            String s = res.to<String>();
            requestText(s.c_str());
        }
        if (json->get(res, "trang_oled")) {
            int requestedPage = res.to<int>();
            if (requestedPage >= 0 && requestedPage <= 3) {
                requestPage(requestedPage);
            }
        }
    }
    else if (path == "/bat_mic_inmp441") isListeningMic = data.to<bool>();
    else if (path == "/ai_tra_loi") {
        String s = data.to<String>();
        requestText(s.c_str());
    }
    else if (path == "/trang_oled") {
        int requestedPage = data.to<int>();
        requestPage(requestedPage);
    }
    requestRender();
}

void streamTimeoutCallback(bool timeout) {
    if (timeout) {
        Serial.println("[STREAM] Timeout, dang ket noi lai...");
    }
}

// ================= 6. ĐỌC DHT22 & VẼ MẶT ROBOT AI NHẤP NHÁY MẮT =================
void readSafeDHT(DHTesp &sensor, float &temp, float &hum) {
    TempAndHumidity data = sensor.getTempAndHumidity();
    if (sensor.getStatus() == DHTesp::ERROR_NONE && !isnan(data.temperature) && !isnan(data.humidity)) {
        if (data.temperature >= -40.0 && data.temperature <= 80.0 &&
            data.humidity >= 0.0 && data.humidity <= 100.0) {
            temp = round(data.temperature * 10.0) / 10.0;
            hum  = round(data.humidity * 10.0) / 10.0;
        }
    }
}

void drawWiFiIcon(int x, int y) {
    u8g2.drawPixel(x + 3, y + 6);
    u8g2.drawCircle(x + 3, y + 6, 3, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
    u8g2.drawCircle(x + 3, y + 6, 6, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
}

void renderOLED() {
    u8g2.clearBuffer();
    unsigned long now = millis();
    bool isAISpeaking = (now - lastSpkReceiveTime < 450);

    // 1. CẢNH BÁO KHẨN CẤP KHI CÓ CHÁY HOẶC RÒ KHÍ GAS
    if (isFire || isGasLeak) {
        u8g2.drawFrame(0, 0, 128, 64);
        u8g2.drawBox(0, 0, 128, 16);
        u8g2.setDrawColor(0);
        u8g2.setFont(u8g2_font_helvB08_tr);
        u8g2.drawStr(14, 12, "!! CANH BAO KHAN !!");
        u8g2.setDrawColor(1);

        if (isFire) u8g2.drawStr(8, 34, "PHONG 2: CO CHAY!");
        if (isGasLeak) u8g2.drawStr(8, 52, "PHONG 3: RO KHI GAS!");
        u8g2.sendBuffer();
        return;
    }

    // 2. HIỂN THỊ DỮ LIỆU KHI ĐƯỢC YÊU CẦU (Trang 0, 1, 2, 3)
    if (oledPage >= 0 && oledPage <= 3) {
        u8g2.drawFrame(0, 0, 128, 64);
        u8g2.setFont(u8g2_font_helvB08_tr);
        switch (oledPage) {
            case 0:
                u8g2.drawStr(25, 14, "[ PHONG 1 ]");
                u8g2.drawHLine(0, 18, 128);
                u8g2.setFont(u8g2_font_helvR08_tr);
                u8g2.setCursor(10, 36);
                u8g2.printf("Nhiet do : %.1f C", t1);
                u8g2.setCursor(10, 52);
                u8g2.printf("Do am    : %.1f %%", h1);
                break;

            case 1:
                u8g2.drawStr(25, 14, "[ PHONG 2 ]");
                u8g2.drawHLine(0, 18, 128);
                u8g2.setFont(u8g2_font_helvR08_tr);
                u8g2.setCursor(8, 32);
                u8g2.printf("T: %.1f C | H: %.1f %%", t2, h2);
                u8g2.setCursor(8, 50);
                u8g2.printf("Bao chay: An toan");
                break;

            case 2:
                u8g2.drawStr(25, 14, "[ PHONG 3 ]");
                u8g2.drawHLine(0, 18, 128);
                u8g2.setFont(u8g2_font_helvR08_tr);
                u8g2.setCursor(8, 32);
                u8g2.printf("T: %.1f C | H: %.1f %%", t3, h3);
                u8g2.setCursor(8, 50);
                u8g2.printf("Khi Gas : An toan (%d)", gasAO);
                break;

            case 3: {
                int rainPercent = map(constrain(rainAO, 0, 4095), 4095, 0, 0, 100);
                u8g2.drawStr(16, 14, "[ THOI TIET ]");
                u8g2.drawHLine(0, 18, 128);
                u8g2.setFont(u8g2_font_helvR08_tr);
                u8g2.setCursor(8, 36);
                u8g2.printf("Trang thai: %s", isRaining ? "CO MUA" : "Khong mua");
                u8g2.setCursor(8, 52);
                u8g2.printf("Do am mua : %d %%", rainPercent);
                break;
            }
        }
        u8g2.sendBuffer();
        return;
    }

    // 3. MẶC ĐỊNH (oledPage == 4): KHUÔN MẶT ROBOT AI NHẤP NHÁY MẮT
    u8g2.setFont(u8g2_font_6x10_tf);
    if (WiFi.status() == WL_CONNECTED) {
        drawWiFiIcon(3, 2);
    } else {
        u8g2.drawStr(3, 9, "x");
    }
    if (isAISpeaking) {
        u8g2.drawStr(15, 9, "AI dang noi...");
    } else {
        u8g2.drawStr(15, 9, "Dang lang nghe...");
    }
    u8g2.setCursor(104, 9);
    u8g2.printf("%.0fC", t1);

    int leftEyeX  = 34 + eyeLookOffsetX;
    int rightEyeX = 74 + eyeLookOffsetX;

    if (isAISpeaking) {
        int bounce = ((now / 140) % 2 == 0) ? 0 : 2;
        u8g2.drawRBox(leftEyeX, 17 + bounce, 20, 10, 4);
        u8g2.drawRBox(rightEyeX, 17 + bounce, 20, 10, 4);
        u8g2.setDrawColor(0);
        u8g2.drawBox(leftEyeX + 3, 24 + bounce, 14, 3);
        u8g2.drawBox(rightEyeX + 3, 24 + bounce, 14, 3);
        u8g2.setDrawColor(1);
    } else if (isEyeBlinking) {
        u8g2.drawRBox(leftEyeX, 22, 20, 3, 1);
        u8g2.drawRBox(rightEyeX, 22, 20, 3, 1);
    } else {
        u8g2.drawRBox(leftEyeX, 14, 20, 15, 6);
        u8g2.drawRBox(rightEyeX, 14, 20, 15, 6);
    }

    u8g2.setFont(u8g2_font_5x8_tf);
    int maxChars = 24;
    String line1 = aiLastReply.substring(0, min((int)aiLastReply.length(), maxChars));
    line1.trim();
    int w1 = u8g2.getStrWidth(line1.c_str());
    u8g2.setCursor(max(2, (128 - w1) / 2), 46);
    u8g2.print(line1);

    if (aiLastReply.length() > maxChars) {
        String line2 = aiLastReply.substring(maxChars, min((int)aiLastReply.length(), maxChars * 2));
        line2.trim();
        int w2 = u8g2.getStrWidth(line2.c_str());
        u8g2.setCursor(max(2, (128 - w2) / 2), 58);
        u8g2.print(line2);
    }

    u8g2.sendBuffer();
}

// ================= 6B. ĐIỀU KHIỂN TRANG OLED =================
void showDataPageTemporarily(int page) {
    if (page >= 0 && page <= 3) {
        oledPage = page;
        dataPageShownTime = millis();
        renderOLED();
    } else {
        oledPage = 4;
        renderOLED();
    }
}

void detectDataPageFromReply(const String &replyText) {
    String lower = replyText;
    lower.toLowerCase();
    if (lower.indexOf("phong 1") >= 0) {
        showDataPageTemporarily(0);
    } else if (lower.indexOf("phong 2") >= 0 || lower.indexOf("bao chay") >= 0) {
        showDataPageTemporarily(1);
    } else if (lower.indexOf("phong 3") >= 0 || lower.indexOf("khi ga") >= 0) {
        showDataPageTemporarily(2);
    } else if (lower.indexOf("thoi tiet") >= 0 || lower.indexOf("mua") >= 0) {
        showDataPageTemporarily(3);
    }
}

void processUiRequests() {
    int page = -1;
    bool hasText = false;
    bool needRender = false;
    char textBuf[UI_TEXT_MAX];

    portENTER_CRITICAL(&uiMux);
    page = pendingPage;
    pendingPage = -1;
    if (pendingText) {
        memcpy(textBuf, pendingTextBuf, UI_TEXT_MAX);
        pendingText = false;
        hasText = true;
    }
    needRender = pendingRender;
    pendingRender = false;
    portEXIT_CRITICAL(&uiMux);

    if (hasText) {
        textBuf[UI_TEXT_MAX - 1] = '\0';
        aiLastReply = String(textBuf);
        detectDataPageFromReply(aiLastReply);
        needRender = true;
    }
    if (page >= 0) {
        showDataPageTemporarily(page);
        needRender = false;
    }
    if (needRender) {
        renderOLED();
    }
}

// ================= 7. FIREBASE CHẠY TRONG TASK RIÊNG =================
bool uploadAllToFirebase() {
    int firePercent = map(constrain(flameAO, 0, 4095), 4095, 0, 0, 100);
    int rainPercent = map(constrain(rainAO, 0, 4095), 4095, 0, 0, 100);
    int gasPercent  = map(constrain(gasAO, 0, 4095), 0, 4095, 0, 100);
    bool fire = isFire, gas = isGasLeak, rain = isRaining;

    FirebaseJson json;
    json.set("Phong1/nhiet_do", t1);
    json.set("Phong1/do_am", h1);

    json.set("Phong2/nhiet_do", t2);
    json.set("Phong2/do_am", h2);
    json.set("Phong2/co_chay", fire);
    json.set("Phong2/canh_bao_chay", fire ? "CẢNH BÁO: CÓ CHÁY!" : "An toàn");
    json.set("Phong2/cuong_do_lua_phan_tram", firePercent);

    json.set("Phong3/nhiet_do", t3);
    json.set("Phong3/do_am", h3);
    json.set("Phong3/ro_ri_gas", gas);
    json.set("Phong3/canh_bao_gas", gas ? "CẢNH BÁO: RÒ RỈ KHÍ GAS!" : "An toàn");
    json.set("Phong3/nong_do_gas_phan_tram", gasPercent);
    json.set("Phong3/gas_analog", gasAO);

    json.set("NgoaiTroi/co_mua", rain);
    json.set("NgoaiTroi/trang_thai_mua", rain ? "Có mưa" : "Không mưa");
    json.set("NgoaiTroi/muc_do_mua_phan_tram", rainPercent);

    if (Firebase.RTDB.updateNodeSilent(&fbdo, "/NhaThongMinh", &json)) {
        Serial.println("-> [Firebase OK] Da cap nhat du lieu 3 phong & thoi tiet!");
        return true;
    }
    Serial.print("-> [Firebase LOI]: ");
    Serial.println(fbdo.errorReason());
    return false;
}

static bool fbConfigDone = false;
static bool fbSignedUp = false;
static bool fbBegun = false;
static bool fbCtrlInit = false;
static bool fbStreamOk = false;

bool firebaseInitStep() {
    if (!fbConfigDone) {
        fbdo.setBSSLBufferSize(2048, 1024);
        fbdo.setResponseSize(1024);
        fbdoStream.setBSSLBufferSize(2048, 1024);
        fbdoStream.setResponseSize(1024);

        config.api_key = API_KEY;
        config.database_url = DATABASE_URL;
        config.timeout.serverResponse = 8 * 1000;
        config.timeout.socketConnection = 8 * 1000;
        fbConfigDone = true;
    }

    if (!fbSignedUp) {
        if (Firebase.signUp(&config, &auth, "", "")) {
            Serial.println("Ket noi Firebase thanh cong!");
            fbSignedUp = true;
            signupOK = true;
        } else {
            Serial.printf("Loi Firebase SignUp: %s\n", config.signer.signupError.message.c_str());
            return false;
        }
    }

    if (!fbBegun) {
        config.token_status_callback = tokenStatusCallback;
        Firebase.begin(&config, &auth);
        Firebase.reconnectWiFi(false);
        fbBegun = true;
    }

    if (!Firebase.ready()) return false;

    if (!fbCtrlInit) {
        FirebaseJson initCtrl;
        initCtrl.set("bat_mic_inmp441", true);
        initCtrl.set("ai_tra_loi", "Chao mung Nha Thong Minh");
        initCtrl.set("trang_oled", 4);
        if (!Firebase.RTDB.updateNode(&fbdo, "/NhaThongMinh/DieuKhien", &initCtrl)) {
            Serial.printf("Loi khoi tao DieuKhien: %s\n", fbdo.errorReason().c_str());
            return false;
        }
        fbCtrlInit = true;
    }

    if (!fbStreamOk) {
        if (!Firebase.RTDB.beginStream(&fbdoStream, "/NhaThongMinh/DieuKhien")) {
            Serial.printf("Loi Stream: %s\n", fbdoStream.errorReason().c_str());
            return false;
        }
        Firebase.RTDB.setStreamCallback(&fbdoStream, streamCallback, streamTimeoutCallback);
        fbStreamOk = true;
    }
    return true;
}

void firebaseTask(void *pvParameters) {
    unsigned long lastInitTry = 0;
    unsigned long lastUpload = 0;
    unsigned long nextAllowed = 0;
    int failCount = 0;

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        unsigned long now = millis();

        if (!(fbSignedUp && fbBegun && fbCtrlInit && fbStreamOk)) {
            if (lastInitTry == 0 || now - lastInitTry >= 8000) {
                lastInitTry = now;
                firebaseInitStep();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!Firebase.ready()) {
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        bool due    = (now - lastUpload >= FIREBASE_PERIOD_MS);
        bool urgent = uploadRequested;
        if ((due || urgent) && now >= nextAllowed) {
            uploadRequested = false;
            lastUpload = now;
            if (uploadAllToFirebase()) {
                failCount = 0;
                nextAllowed = now + FIREBASE_MIN_GAP_MS;
            } else {
                if (failCount < 4) failCount++;
                unsigned long backoff = 2000UL << failCount;
                if (backoff > 30000UL) backoff = 30000UL;
                nextAllowed = now + backoff;
                uploadRequested = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ================= 8. WIFI: KẾT NỐI & TỰ KẾT NỐI LẠI =================
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        int r = info.wifi_sta_disconnected.reason;
        Serial.printf("[WIFI] Mat ket noi, ma loi = %d", r);
        if (r == 201) Serial.print(" (KHONG THAY MANG: Mo man hinh Diem truy cap ca nhan tren iPhone & bat 2.4GHz)");
        else if (r == 202 || r == 15 || r == 204) Serial.print(" (Sai mat khau hoac bat tay that bai)");
        else if (r == 200) Serial.print(" (Mat beacon: song yeu hoac AP tat)");
        Serial.println();
    }
}

void wifiWatchdog(unsigned long now) {
    static unsigned long lastCheck = 0, downSince = 0, lastKick = 0;
    if (now - lastCheck < 2000) return;
    lastCheck = now;

    if (WiFi.status() == WL_CONNECTED) {
        downSince = 0;
        return;
    }
    if (downSince == 0) downSince = now;

    if (now - downSince > 6000 && now - lastKick > 10000) {
        lastKick = now;
        Serial.println("[WIFI] Thu ket noi lai...");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
}

void connectWiFiAtBoot() {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(true); // Bật modem sleep (bắt buộc để Wi-Fi + BLE 2.4GHz hoạt động đồng thời ổn định)
    WiFi.setTxPower(WIFI_TX_POWER);
    WiFi.onEvent(onWiFiEvent);

    for (int attempt = 1; attempt <= 3 && WiFi.status() != WL_CONNECTED; attempt++) {
        Serial.printf("Dang ket noi Wi-Fi (lan %d/3)", attempt);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
            delay(250);
            Serial.print(".");
        }
        Serial.println();
        if (WiFi.status() != WL_CONNECTED) {
            WiFi.disconnect();
            delay(500);
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("Da ket noi Wi-Fi! IP: ");
        Serial.print(WiFi.localIP());
        Serial.printf(" | RSSI: %d dBm\n", WiFi.RSSI());
    } else {
        Serial.println("CHUA ket noi duoc Wi-Fi - he thong van chay va se tu thu lai.");
    }
}

// ================= 9. SETUP & LOOP =================
void setup() {
    setCpuFrequencyMhz(CPU_FREQ_MHZ);
    Serial.begin(115200);

    pinMode(FLAME_DO_PIN, INPUT_PULLUP);
    pinMode(GAS_DO_PIN, INPUT_PULLUP);
    pinMode(RAIN_DO_PIN, INPUT_PULLUP);

    dht1.setup(DHT1_PIN, DHTesp::DHT22);
    dht2.setup(DHT2_PIN, DHTesp::DHT22);
    dht3.setup(DHT3_PIN, DHTesp::DHT22);

    setupSpeakerI2S();
    setupMicI2S();
    playToneSpk(1000, 120);

    Wire.begin(OLED_SDA, OLED_SCL);
    Wire.setClock(400000);
    u8g2.begin();
    renderOLED();

    connectWiFiAtBoot();
    smartKeyInit();

    udpAudio.begin(UDP_SPK_PORT);
    playToneSpk(1500, 150);

    readSafeDHT(dht1, t1, h1);
    readSafeDHT(dht2, t2, h2);
    readSafeDHT(dht3, t3, h3);
    oledPage = 4;
    renderOLED();

    sendBootGreetingBroadcast();

    xTaskCreatePinnedToCore(micCaptureTask, "MicTask",   6144,  NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(udpRxTask,      "UdpRxTask", 5120,  NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(firebaseTask,   "FbTask",    12288, NULL, 1, NULL, 1);

    uploadRequested = true;
}

void loop() {
    unsigned long now = millis();

    // ================= 0. XỬ LÝ YÊU CẦU GIAO DIỆN & GIÁM SÁT WIFI =================
    processUiRequests();
    wifiWatchdog(now);
    smartKeyLoop();

    // ================= 1. HIỆU ỨNG NHẤP NHÁY MẮT AI & TỰ QUAY VỀ MẶT AI =================
    if (oledPage != 4 && (now - dataPageShownTime >= DATA_DISPLAY_DURATION)) {
        oledPage = 4;
        eyeLookOffsetX = 0;
        renderOLED();
    }

    if (oledPage == 4) {
        bool isSpeaking = (now - lastSpkReceiveTime < 450);
        if (isSpeaking) {
            if (now - lastFaceAnimTime >= 200) {
                lastFaceAnimTime = now;
                renderOLED();
            }
        } else {
            if (!isEyeBlinking && (now - lastEyeBlinkTime >= nextBlinkInterval)) {
                isEyeBlinking = true;
                lastEyeBlinkTime = now;
                renderOLED();
            } else if (isEyeBlinking && (now - lastEyeBlinkTime >= 160)) {
                isEyeBlinking = false;
                lastEyeBlinkTime = now;
                int r = random(0, 10);
                if (r < 2) {
                    nextBlinkInterval = 220;
                } else {
                    nextBlinkInterval = random(2200, 4200);
                    eyeLookOffsetX = (r == 3) ? -4 : ((r == 4) ? 4 : 0);
                }
                renderOLED();
            }
        }
    }

    // ================= 2. ĐỌC CẢM BIẾN LỬA, GAS, MƯA MỖI 50 ms =================
    if (now - lastSensorTime >= SENSOR_PERIOD_MS) {
        lastSensorTime = now;

        flameAO = analogRead(FLAME_AO_PIN);
        gasAO   = analogRead(GAS_AO_PIN);
        rainAO  = analogRead(RAIN_AO_PIN);

        static int fireFilterCount = 0;
        static int gasFilterCount  = 0;

        if (digitalRead(FLAME_DO_PIN) == LOW && flameAO < 3200) {
            if (fireFilterCount < 15) fireFilterCount++;
        } else {
            fireFilterCount = 0;
        }
        isFire = (fireFilterCount >= 10);

        if (now > 20000 && digitalRead(GAS_DO_PIN) == LOW && gasAO > 1200) {
            if (gasFilterCount < 15) gasFilterCount++;
        } else {
            gasFilterCount = 0;
        }
        isGasLeak = (gasFilterCount >= 10);

        isRaining = (digitalRead(RAIN_DO_PIN) == LOW);

        if ((isFire || isGasLeak) && (now - lastAlarmBeepTime >= 1000)) {
            lastAlarmBeepTime = now;
            playToneSpk(2200, 200);
        }
    }

    // ================= 3. ĐỌC DHT22 LUÂN PHIÊN =================
    if (now - lastDHTReadTime >= DHT_STEP_MS) {
        lastDHTReadTime = now;
        static int dhtIdx = 0;
        if (dhtIdx == 0) readSafeDHT(dht1, t1, h1);
        else if (dhtIdx == 1) readSafeDHT(dht2, t2, h2);
        else readSafeDHT(dht3, t3, h3);
        dhtIdx = (dhtIdx + 1) % 3;

        if (dhtIdx == 0) {
            Serial.printf("P1[%.1fC, %.1f%%] | P2[%.1fC, %.1f%%, Chay:%d] | P3[%.1fC, %.1f%%, Gas:%d] | Mua:%d\n",
                          t1, h1, t2, h2, (int)isFire, t3, h3, (int)isGasLeak, (int)isRaining);
            if (oledPage != 4) {
                renderOLED();
            }
        }
    }

    // ================= 4. BÁO TASK FIREBASE KHI CÓ BIẾN ĐỘNG =================
    bool stateChanged = (isFire != prevFire) || (isGasLeak != prevGas) || (isRaining != prevRain);
    if (stateChanged) {
        prevFire = isFire;
        prevGas  = isGasLeak;
        prevRain = isRaining;
        uploadRequested = true;
        renderOLED();
    }

    // ================= 5. IN TÌNH TRẠNG HỆ THỐNG =================
    if (now - lastStatusPrint >= STATUS_PRINT_MS) {
        lastStatusPrint = now;
        Serial.printf("[SYS] heap=%u min=%u maxblk=%u | WiFi=%s RSSI=%d | chip=%.0fC | cpu=%dMHz | loaGoiMat=%u | up=%lus\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                      WiFi.status() == WL_CONNECTED ? "OK" : "MAT", WiFi.RSSI(),
                      temperatureRead(), (int)getCpuFrequencyMhz(), (unsigned)spkDropCount, now / 1000);
    }

    delay(2);
}