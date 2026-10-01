#include <Arduino.h>
#include <LovyanGFX.hpp>
#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"

// ========== 摄像头引脚（官方 kevin-sp-v3-dev/config.h） ==========
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     4
#define SIOC_GPIO_NUM     5
#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM       8
#define Y3_GPIO_NUM       9
#define Y2_GPIO_NUM       11
#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM     13

// ========== TFT 引脚 ==========
#define TFT_CS   45
#define TFT_DC   48
#define TFT_RST  21
#define TFT_MOSI 20
#define TFT_SCLK 19
#define TFT_BL   38

// ========== 按钮 ==========
#define BTN_S1   0    // 拍照
#define BTN_S2   3    // 开始/停止录像

// ========== SD 卡（1-bit 模式） ==========
#define SD_CLK   39
#define SD_CMD   38
#define SD_D0    40

// ========== 屏幕驱动 ==========
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_SPI      _bus_instance;
public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = 1;
      cfg.pin_sclk    = TFT_SCLK;
      cfg.pin_mosi    = TFT_MOSI;
      cfg.pin_miso    = -1;
      cfg.pin_dc      = TFT_DC;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs           = TFT_CS;
      cfg.pin_rst          = TFT_RST;
      cfg.pin_busy         = -1;
      cfg.panel_width      = 240;
      cfg.panel_height     = 320;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = true;
      cfg.invert           = true;   // 颜色反了改成 false
      cfg.rgb_order        = false;  // 红蓝颠倒改成 true
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

LGFX tft;

// ========== 状态 ==========
enum Mode { MODE_DISPLAY, MODE_RECORD };
Mode currentMode = MODE_DISPLAY;

bool recording = false;
File videoFile;
int photoCounter = 0;
int videoCounter = 0;

// ========== 背光控制（关键：释放 GPIO38） ==========
void backlight(bool on) {
  if (on) {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
  } else {
    pinMode(TFT_BL, INPUT);  // 高阻态，彻底让出 GPIO 38
  }
}

// ========== 摄像头初始化 ==========
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk  = XCLK_GPIO_NUM;
  config.pin_pclk  = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href  = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn  = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_QVGA;  // 320x240
  config.jpeg_quality = 12;
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }
  return true;
}

// ========== SD 卡按需挂载 ==========
bool sdMounted = false;

bool mountSD() {
  if (sdMounted) return true;
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true, true, SDMMC_FREQ_DEFAULT, 5)) {
    Serial.println("SD mount failed");
    return false;
  }
  sdMounted = true;
  Serial.printf("SD mounted, size=%lluMB\n", SD_MMC.cardSize() / (1024 * 1024));
  return true;
}

void unmountSD() {
  if (!sdMounted) return;
  SD_MMC.end();
  sdMounted = false;
  Serial.println("SD unmounted");
}

// ========== 拍照 ==========
bool capturePhoto() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return false;

  char path[64];
  snprintf(path, sizeof(path), "/PHOTO_%03d.jpg", ++photoCounter);
  File f = SD_MMC.open(path, FILE_WRITE);
  bool ok = false;
  if (f) {
    f.write(fb->buf, fb->len);
    f.close();
    ok = true;
    Serial.printf("Saved %s (%u bytes)\n", path, fb->len);
  }
  esp_camera_fb_return(fb);
  return ok;
}

// ========== 显示状态条 ==========
void drawStatus() {
  tft.fillRect(0, tft.height() - 22, tft.width(), 22, TFT_BLACK);
  tft.setTextColor(recording ? TFT_RED : TFT_GREEN, TFT_BLACK);
  tft.setCursor(4, tft.height() - 18);
  tft.printf("%s  P:%d  V:%d",
             recording ? "REC" : "READY",
             photoCounter, videoCounter);
}

// ========== 模式切换 ==========
void enterDisplayMode() {
  currentMode = MODE_DISPLAY;
  backlight(true);
  drawStatus();
  Serial.println("[MODE] DISPLAY");
}

void doPhoto() {
  // 关背光 → 释放 GPIO38
  backlight(false);
  delay(50);

  if (mountSD()) {
    capturePhoto();
    unmountSD();
  } else {
    Serial.println("SD not available, photo skipped");
  }

  delay(100);
  backlight(true);

  // 清空残留帧
  camera_fb_t *dummy = esp_camera_fb_get();
  if (dummy) esp_camera_fb_return(dummy);

  drawStatus();
}

void startRecord() {
  backlight(false);
  delay(50);

  if (!mountSD()) {
    Serial.println("SD mount failed, cannot record");
    backlight(true);
    return;
  }

  char path[64];
  snprintf(path, sizeof(path), "/VIDEO_%03d.mjpeg", ++videoCounter);
  videoFile = SD_MMC.open(path, FILE_WRITE);
  if (!videoFile) {
    Serial.println("Failed to open video file");
    unmountSD();
    backlight(true);
    return;
  }
  recording = true;
  currentMode = MODE_RECORD;
  Serial.printf("[MODE] RECORD -> %s\n", path);
}

void stopRecord() {
  recording = false;
  if (videoFile) videoFile.close();
  unmountSD();
  delay(100);
  backlight(true);

  camera_fb_t *dummy = esp_camera_fb_get();
  if (dummy) esp_camera_fb_return(dummy);

  enterDisplayMode();
  Serial.println("[MODE] RECORD stopped");
}

// ========== setup ==========
void setup() {
  Serial.begin(115200);
  pinMode(BTN_S1, INPUT_PULLUP);
  pinMode(BTN_S2, INPUT_PULLUP);

  backlight(true);
  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_BLACK);

  initCamera();
  enterDisplayMode();
}

// ========== loop ==========
void loop() {
  // ---- S1：拍照 ----
  if (digitalRead(BTN_S1) == LOW) {
    delay(30);
    if (digitalRead(BTN_S1) == LOW && !recording) {
      doPhoto();
      while (digitalRead(BTN_S1) == LOW) delay(10);
    }
  }

  // ---- S2：开始 / 停止录像 ----
  if (digitalRead(BTN_S2) == LOW) {
    delay(30);
    if (digitalRead(BTN_S2) == LOW) {
      if (recording) stopRecord();
      else           startRecord();
      while (digitalRead(BTN_S2) == LOW) delay(10);
    }
  }

  // ---- 按模式执行 ----
  if (currentMode == MODE_DISPLAY) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      tft.drawJpg(fb->buf, fb->len, 0, 0);
      esp_camera_fb_return(fb);
    }
    delay(30);
  } else if (currentMode == MODE_RECORD) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      if (videoFile) videoFile.write(fb->buf, fb->len);
      esp_camera_fb_return(fb);
    }
    delay(50);  // 约 20 FPS
  }
}
