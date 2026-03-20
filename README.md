# XIAO ESP32S3 Sense RTSP Server & WebUI

這個專案專為 **Seeed Studio XIAO ESP32S3 Sense** 開發板設計，提供穩定且高效能的 RTSP 影像串流與現代化的網頁配置介面。

## 🌟 核心特色
1. **雙核協同非同步推流 (Dual-Core Architecture)**
   - `Core 1` 負責處理 WebUI 請求與 WiFi 底層連線，保持介面暢通不卡頓。
   - `Core 0` 獨立處理高負載的相機截取 (OV2640) 與 RTSP 影像封包推流。有效避免 WatchDog 當機。
2. **解決硬體腳位衝突 (Pin Conflict Fixed)**
   - 更正原廠正確的 DVP 影像腳位 (Y2=15)。
   - 初始化相機前強制拉高 MicroSD 的 CS 腳位 (`GPIO 21`)，**徹底解決相機受到 SD 卡干擾導致無法截取影像的致命問題**！
3. **內建零配置 Web UI (Web Interface)**
   - 簡易明瞭的配置頁面可顯示串流狀態、RTSP 連線網址、NTP 校時。
   - 內建**「拍照即時快照」**按鈕，點擊後不會刷新頁面，直接擷取相機最新測試畫面。
4. **低延遲與高效能緩衝 (Performance Tweaks)**
   - 關閉 `WiFi Sleep` 杜絕網路封包掉幀與延遲。
   - 利用 ESP32-S3 `PSRAM` 自動啟用雙幀緩衝 (Double Buffering)。
   - 優化傳輸至 `SVGA (800x600)` 解析度，維持流暢 `~15 FPS` 體驗。

## 🛠️ 開發與安裝指南

### 硬體需求
- Seeed Studio XIAO ESP32-S3 Sense
- 支援 2.4GHz WiFi 環境

### 環境設置
1. 開啟 Arduino IDE，安裝 `esp32` 開發板套件。
2. 開發板選擇 `XIAO_ESP32S3`，記得開啟 **PSRAM** 選項 (`OPI PSRAM`)。
3. 下載本專案並使用 Arduino IDE 開啟 `rtsp.ino`，程式資料夾內已自動包含所需的修改版 `CRtspSession`, `OV2640Streamer` 驅動程式，**不需額外尋找微型 RTSP 函式庫**。

### 使用者配置
開啟 `rtsp.ino` 修改最上方的網路設定：
```cpp
const char* WIFI_SSID = "您的_WIFI_名稱";
const char* WIFI_PASSWORD = "您的_WIFI_密碼";
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 28800; //預設為 UTC+8 台灣時間
```

## 🎥 觀看串流
燒錄並開啟 Serial Monitor (`包率 115200`)，您將看到初始化狀態日誌與自動分配的 IP：
1. **即時觀看 Web UI**：在瀏覽器輸入您的 IP (如 `http://192.168.1.50`)。
2. **使用播放器觀看串流**：開啟 VLC 播放器或 PotPlayer，貼上 RTSP 網址：
   ```
   rtsp://<您的IP>:554/mjpeg/1
   ```

## 📄 授權條款
本專案改寫自 Micro-RTSP，程式碼開源提供學術研究與硬體愛好者自由修改。
