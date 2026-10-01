#include <Arduino.h>
#include <LovyanGFX.hpp>
#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include <driver/i2s.h>

// ========== 摄像头引脚（来自官方 kevin-sp-v3-dev/config.h） ==========
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

// ========== TFT 引脚（根据扩展板） ==========
#define TFT_CS   45
#define TFT_DC   48
#define TFT_RST  21
#define TFT_MOSI 20
#define TFT_SCLK 19
#define TFT_BL   38

// ========== 按钮 ==========
#define BTN_S1   0    // 拍照
#define BTN_S2   3    // 录像

// ========== SD 卡引脚（1-bit 模式） ==========
#define SD_CLK   39
#define SD_CMD   38
#define SD_D0    40

// ========== 麦克风 I2S 引脚（必须核实！） ==========
#define I2S_SCK   32
#define I2S_WS    33
#define I2S_SD    25
#define SAMPLE_RATE 16000

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
      cfg.invert           = true;
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

LGFX tft;

// ========== 模式状态机 ==========
enum Mode { MODE_DISPLAY, MODE_PHOTO, MODE_RECORD };
Mode currentMode = MODE_DISPLAY;

bool recording = false;
File videoFile;
File audioFile;
int photoCounter = 0;
int videoCounter = 0;

// ========== 背光控制 ==========
void backlight(bool on) {
  if (on) {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
  } else {
    pinMode(TFT_BL, INPUT); // 高阻态，彻底释放 GPIO 38
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
  config.frame_size   = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  return (err == ESP_OK);
}

// ========== SD 卡按需初始化 ==========
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

// ========== 音频（I2S 麦克风） ==========
bool initAudio() {
  i2s_config_t i2s_cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 256,
    .use_apll = false,
  };
  if (i2s_driver_install(I2S_NUM_0, &i2s_cfg, 0, NULL) != ESP_OK) return false;
  i2s_pin_config_t pin_cfg = {
    .bck_io_num   = I2S_SCK,
    .ws_io_num    = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_SD,
  };
  return (i2s_set_pin(I2S_NUM_0, &pin_cfg) == ESP_OK);
}

void deinitAudio() {
  i2s_driver_uninstall(I2S_NUM_0);
}

// ========== 拍一张照片 ==========
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

// ========== 模式切换 ==========
void enterDisplayMode() {
  currentMode = MODE_DISPLAY;
  backlight(true);
  Serial.println("[MODE] DISPLAY");
}

void enterPhotoMode() {
  currentMode = MODE_PHOTO;
  backlight(false);
  delay(50);
  if (mountSD()) {
    capturePhoto();
    unmountSD();
  } else {
    Serial.println("SD not available, photo skipped");
  }
  delay(100);
  enterDisplayMode();
}

void enterRecordMode() {
  currentMode = MODE_RECORD;
  backlight(false);
  delay(50);

  if (!mountSD()) {
    Serial.println("SD mount failed, cannot record");
    enterDisplayMode();
    return;
  }
  if (!initAudio()) {
    Serial.println("Audio init failed, video only");
  }

  char path[64];
  snprintf(path, sizeof(path), "/VIDEO_%03d.mjpeg", ++videoCounter);
  videoFile = SD_MMC.open(path, FILE_WRITE);
  if (!videoFile) {
    Serial.println("Failed to open video file");
    deinitAudio();
    unmountSD();
    enterDisplayMode();
    return;
  }

  // 创建 WAV 文件（音频）
  char wavPath[64];
  snprintf(wavPath, sizeof(wavPath), "/AUDIO_%03d.wav", videoCounter);
  audioFile = SD_MMC.open(wavPath, FILE_WRITE);
  if (audioFile) {
    // 先写 44 字节占位头，后面补
    uint8_t header[44] = {0};
    audioFile.write(header, 44);
  }

  recording = true;
  Serial.printf("[MODE] RECORD -> %s / %s\n", path, wavPath);
}

void exitRecordMode() {
  recording = false;
  if (videoFile) videoFile.close();
  if (audioFile) {
    // 补写 WAV 头
    audioFile.seek(0);
    // ... 此处省略 WAV 头填充，可简化为直接使用原始 PCM
    audioFile.close();
  }
  deinitAudio();
  unmountSD();
  Serial.println("[MODE] RECORD stopped");
  enterDisplayMode();
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
  // ---- S1: 拍照 ----
  if (digitalRead(BTN_S1) == LOW) {
    delay(30);
    if (digitalRead(BTN_S1) == LOW && currentMode != MODE_RECORD) {
      enterPhotoMode();
      while (digitalRead(BTN_S1) == LOW) delay(10);
    }
  }

  // ---- S2: 开始/停止录像 ----
  if (digitalRead(BTN_S2) == LOW) {
    delay(30);
    if (digitalRead(BTN_S2) == LOW) {
      if (currentMode == MODE_RECORD) {
        exitRecordMode();
      } else {
        enterRecordMode();
      }
      while (digitalRead(BTN_S2) == LOW) delay(10);
    }
  }

  // ---- 按模式执行 ----
  switch (currentMode) {
    case MODE_DISPLAY: {
      camera_fb_t *fb = esp_camera_fb_get();
      if (fb) {
        tft.drawJpg(fb->buf, fb->len, 0, 0);
        esp_camera_fb_return(fb);
      }
      delay(30);
      break;
    }
    case MODE_RECORD: {
      camera_fb_t *fb = esp_camera_fb_get();
      if (fb) {
        if (videoFile) videoFile.write(fb->buf, fb->len);
        esp_camera_fb_return(fb);
      }
      if (audioFile) {
        size_t bytes_read = 0;
        uint8_t audio_buf[512];
        i2s_read(I2S_NUM_0, audio_buf, sizeof(audio_buf), &bytes_read, 0);
        if (bytes_read > 0) audioFile.write(audio_buf, bytes_read);
      }
      delay(40);
      break;
    }
    case MODE_PHOTO:
    default:
      delay(10);
      break;
  }
}
