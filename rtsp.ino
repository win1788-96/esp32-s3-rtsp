#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include "camera_pins.h"

// ===== 如果出現編譯錯誤，請確認您已經從 Arduino IDE 的「管理程式庫」安裝了 "Micro-RTSP" 函式庫 =====
// 搜尋 "Micro-RTSP" 作者為 Kevin Hester
#include "CRtspSession.h"
#include "OV2640Streamer.h"

// --- 使用者設定 ---
const char* WIFI_SSID = "maggie888";
const char* WIFI_PASSWORD = "aaaa222288";
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 28800; // 台灣時區 UTC+8
const int   DAYLIGHT_OFFSET_SEC = 0;

// Web 設定介面與 RTSP Server
WebServer webServer(80);
WiFiServer rtspServer(554);

// RTSP 相關指標 (Micro-RTSP)
CStreamer *streamer = NULL;
CRtspSession *session = NULL;
WiFiClient rtspClient;

// FreeRTOS Task Handles
TaskHandle_t streamTaskHandle = NULL;

// 初始化相機
void setupCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM; // 21 (使用者已確認)
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    
    // 串流頻寬控制：解析度設為 SVGA (800x600) 以維持良好幀率
    config.frame_size = FRAMESIZE_SVGA; 
    config.pixel_format = PIXFORMAT_JPEG;
    
    // 利用 ESP32S3 的 PSRAM 進行雙緩衝，提升雙核效能與避免影像撕裂
    if (psramFound()) {
        config.jpeg_quality = 10;
        config.fb_count = 2; // 開啟雙幀緩衝
        config.grab_mode = CAMERA_GRAB_LATEST; 
        Serial.println("[相機日誌] 偵測到 PSRAM！已啟用雙幀緩衝 (Double Buffering) 與高畫質設定。");
    } else {
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
        Serial.println("[相機日誌] ⚠️ 未偵測到 PSRAM，使用單幀緩衝與一般畫質。");
    }

    // 關鍵硬體防衝突：XIAO S3 Sense 的 SD 卡 CS 是 GPIO 21
    // 如果 SD 卡插著而 CS 處於浮接或低電平狀態，SD 卡會干擾相機的資料傳輸導致無法讀取畫面
    pinMode(21, OUTPUT);
    digitalWrite(21, HIGH);  // 強制關閉 SD 卡的 SPI 通訊
    Serial.println("[硬體修復] 已將 SD 卡 CS (GPIO 21) 拉高以防止與相機互相干擾。");

    Serial.println("[相機日誌] 正在初始化相機模組...");
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[相機日誌] ❌ 相機初始化失敗！錯誤碼: 0x%x\n", err);
        return;
    }
    Serial.println("[相機日誌] ✅ 相機初始化成功！");
}

// =======================================================
// 核心0：RTSP 串流處理專屬任務
// 將相機截取與網路串流放於獨立核心，不干擾 Core 1 的系統與Web
// =======================================================
void rtspTask(void * pvParameters) {
    uint32_t msecPerFrame = 1000 / 15; // 目標幀率 15 FPS
    Serial.println("[RTSP 任務] ✅ RTSP 守護行程已於 Core 0 成功啟動，等待連線...");
    
    while (true) {
        uint32_t currentMsec = millis();
        
        // 1. 處理新的 RTSP 客戶端連線
        WiFiClient newClient = rtspServer.accept();
        if (newClient) {
            Serial.print("[RTSP 日誌] 🌐 偵測到新 RTSP 用戶連線，來源 IP: ");
            Serial.println(newClient.remoteIP());
            
            // 清理舊的連線
            if (session) {
                Serial.println("[RTSP 日誌] ⚠️ 清除先前的連線狀態...");
                delete session;
                session = NULL;
            }
            if (streamer) {
                delete streamer;
                streamer = NULL;
            }
            rtspClient = newClient;
            
            // 為新客戶端建立串流器與會話
            Serial.println("[RTSP 日誌] 🛠️ 正在為新用戶建立串流會話 (RTSP Session)...");
            
            // OV2640Streamer 我們已改寫成直接取得鏡頭 Frame，並在此填入 SVGA 的寬高 (800x600)
            streamer = new OV2640Streamer(800, 600);
            // CRtspSession 需要 SOCKET 型別 (ESP32 上為 WiFiClient*)
            session = new CRtspSession(&rtspClient, streamer);
            Serial.println("[RTSP 日誌] ▶️ 串流準備就緒，開始接收客戶端握手協議！");
        }

        // 2. 維持與處理現有的 RTSP 會話
        if (session) {
            session->handleRequests(0); 
            // handleRequests 會處理 OPTIONS, DESCRIBE, SETUP, PLAY 等交握
            
            if (!rtspClient.connected()) {
                Serial.println("[RTSP 日誌] 🛑 RTSP 客戶端已斷線或中止觀看。停止推流。");
                delete session;
                session = NULL;
                delete streamer;
                streamer = NULL;
            } else {
                // 推送畫面給正在觀看 (PLAY state) 的用戶
                streamer->streamImage(currentMsec);
            }
        }

        // 3. 幀率控制延遲，避免佔用100% CPU觸發 Watchdog
        uint32_t msElapsed = millis() - currentMsec;
        if (msecPerFrame > msElapsed) {
            vTaskDelay((msecPerFrame - msElapsed) / portTICK_PERIOD_MS);
        } else {
            vTaskDelay(1 / portTICK_PERIOD_MS); 
        }
    }
}

// 擷取單張相片以供 WebUI 驗證
void handleCapture() {
    Serial.println("[Web 日誌] 📷 接收到相片擷取請求 (/capture)");
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[相機日誌] ❌ 擷取相片失敗！請檢查硬體。");
        webServer.send(500, "text/plain", "Camera Capture Failed");
        return;
    }
    
    // 將 JPEG 資料作為 HTTP 回傳
    webServer.setContentLength(fb->len);
    webServer.send(200, "image/jpeg", "");
    WiFiClient client = webServer.client();
    client.write(fb->buf, fb->len);
    
    esp_camera_fb_return(fb);
    Serial.println("[相機日誌] ✅ 成功回傳單張驗證畫面。");
}

// 簡單的 Web 配置介面首頁
void handleRoot() {
    Serial.println("[Web 日誌] 🌍 接收到 WebUI 頁面請求 (/)，正在回傳系統資訊...");
    String html = "<html><head><meta charset='utf-8'><title>XIAO S3 Sense 配置</title>";
    html += "<style>";
    html += "body{font-family: Arial, sans-serif; padding: 20px; background-color: #f4f4f9; color: #333;} ";
    html += "img{max-width: 100%; height: auto; border: 3px solid #ddd; border-radius: 8px; margin-top: 15px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); display: block;} ";
    html += ".btn{background-color: #4CAF50; color: white; padding: 12px 24px; font-size: 16px; border: none; border-radius: 5px; cursor: pointer; box-shadow: 0 4px 6px rgba(0,0,0,0.1); transition: 0.3s;} ";
    html += ".btn:hover{background-color: #45a049;} ";
    html += ".btn:active{background-color: #3e8e41; box-shadow: 0 2px 4px rgba(0,0,0,0.1); transform: translateY(2px);} ";
    html += "</style>";
    html += "<script>";
    html += "function takePhoto() {";
    html += "  var img = document.getElementById('snapshot');";
    html += "  var btn = document.getElementById('capBtn');";
    html += "  btn.innerText = '📷 拍攝中...';";
    html += "  btn.style.backgroundColor = '#FF9800';";
    html += "  img.src = '/capture?t=' + new Date().getTime();";
    html += "  img.onload = function() { btn.innerText = '📸 拍一張即時照片'; btn.style.backgroundColor = '#4CAF50'; };";
    html += "  img.onerror = function() { btn.innerText = '❌ 拍攝失敗'; btn.style.backgroundColor = '#f44336'; };";
    html += "}";
    html += "</script>";
    html += "</head><body>";
    html += "<h1>XIAO ESP32S3 Sense RTSP 串流中心</h1>";
    html += "<h3>系統狀態: <span style='color: green;'>執行中</span></h3>";
    html += "<p><b>解析度:</b> SVGA (800x600) &nbsp; | &nbsp; <b>FPS:</b> 約 15 fps</p>";
    html += "<p><b>RTSP 網址:</b> <code>rtsp://" + WiFi.localIP().toString() + ":554/mjpeg/1</code></p>";
    
    html += "<h3>📸 相機即時快照驗證</h3>";
    html += "<button id='capBtn' class='btn' onclick='takePhoto()'>📸 拍一張即時照片</button>";
    html += "<img id='snapshot' src=\"/capture\" alt=\"相機畫面讀取中... 請點擊上方拍照按鈕\">";
    
    html += "<hr>";
    
    struct tm timeinfo;
    if(getLocalTime(&timeinfo)){
        html += "<p>NTP 時間: " + String(asctime(&timeinfo)) + "</p>";
    } else {
        html += "<p>NTP 時間: 同步中...</p>";
    }
    
    html += "<p><i>* 建議使用 VLC 或 PotPlayer 開啟上方 RTSP 網址進行連續影音串流。此網頁僅供單張畫面手動測試。</i></p>";
    html += "</body></html>";
    webServer.send(200, "text/html", html);
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n========================================");
    Serial.println("[系統日誌] 🚀 XIAO ESP32S3 Sense RTSP 伺服器啟動中...");
    Serial.println("========================================");

    // 1. 底層配置與相機初始化
    Serial.println("[系統日誌] 步驟 1: 配置硬體與相機腳位...");
    setupCamera();

    // 2. WiFi 連線效能優化
    Serial.println("[系統日誌] 步驟 2: 關閉 WiFi 休眠並連接熱點...");
    WiFi.setSleep(false); // [關鍵優化] 關閉 WiFi 休眠以避免 RTSP 延遲與封包遺失
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("[WiFi 日誌] 等待 WiFi 連線 ");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[WiFi 日誌] ✅ WiFi 連線成功！");
    Serial.print("[WiFi 日誌] 📍 設備 IP 位址: ");
    Serial.println(WiFi.localIP());

    // 3. 系統時間同步 (NTP)
    Serial.print("[系統日誌] 步驟 3: 向 ");
    Serial.print(NTP_SERVER);
    Serial.println(" 請求 NTP 時間同步...");
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    Serial.println("[系統日誌] ⏳ NTP 時區配置完成。");

    // 4. 初始化 WebUI
    Serial.println("[系統日誌] 步驟 4: 啟動 Web 伺服器 (Port 80)...");
    webServer.on("/", HTTP_GET, handleRoot);
    webServer.on("/capture", HTTP_GET, handleCapture);
    webServer.begin();
    Serial.println("[系統日誌] ✅ 網頁伺服器已就緒。");

    // 5. 初始化 RTSP Server
    Serial.println("[系統日誌] 步驟 5: 啟動 RTSP 伺服器 (Port 554)...");
    rtspServer.begin();
    Serial.println("[系統日誌] ✅ RTSP 連線端點建立完成。");
    
    // 6. 核心指派 (Core 0: RTSP 串流專用任務)
    // ESP32S3 的 Arduino 迴圈(loop) 預設在 Core 1，我們將 RTSP 大量資料流處理丟去 Core 0
    Serial.println("[系統日誌] 步驟 6: 分配雙核多工任務 (將 RTSP 推流掛載至 Core 0)...");
    xTaskCreatePinnedToCore(
        rtspTask,
        "rtspTask",
        4096,  // 任務記憶體配置
        NULL,
        1,     // 優先權
        &streamTaskHandle,
        0      // 指定在 Core 0 執行
    );
    Serial.println("[系統日誌] 🎉 全部初始化完畢！系統正在穩定執行中。");
}

void loop() {
    // 主核心 Core 1 負責輕量任務：處理 WebUI 請求、保持系統存活
    webServer.handleClient();
    vTaskDelay(10 / portTICK_PERIOD_MS); // 必要的休眠，餵狗 (Watchdog)
}
