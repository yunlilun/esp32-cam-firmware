/**********************************************************************
  Filename    : Camera Web Server / 摄像头Web服务器
  Description : ESP32S3摄像头图像在网页上显示
  Auther      : Zhu Wenqian
  Modification: 2026-02-04
  代码来源 / Code Source:
  - 基于Freenove ESP32-S3 Camera Example修改
  - 集成Espressif CameraWebServer (Apache License 2.0)
  许可证 / License: CC BY-NC-SA 3.0
  详情 / Details: https://creativecommons.org/licenses/by-nc-sa/3.0/
**********************************************************************/
#include "esp_camera.h"
#include <WiFi.h>
#include <time.h>
#include "sd_read_write.h"
#include "auth.h"
#include "servo_control.h"
#include "ota_server.h"
#include "led_control.h"

// ===================
// Select camera model
// ===================
#define CAMERA_MODEL_ESP32S3_EYE

#include "camera_pins.h"

// ===================
// 运行时长统计
// ===================
unsigned long startTime = 0;

void startCameraServer();
void videoRecordTask(void *pvParameters);

unsigned long getUptimeSeconds() {
  return (millis() - startTime) / 1000;
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  // 初始化LED
  Serial.println("Initializing LED...");
  led_init();
  led_set_status(LED_INIT_SLOW_FLASH);

  // 初始化认证模块
  if(!auth_init()) {
    Serial.println("Failed to initialize auth module");
  } else {
    Serial.println("Auth module initialized");
  }

  // ========== 舵机初始化（已注释，避免卡死） ==========
  // Serial.println("Initializing pan-tilt servos...");
  // if(servo_init()) {
  //   Serial.println("Pan-tilt servos initialized");
  // } else {
  //   Serial.println("Failed to initialize pan-tilt servos");
  // }
  // ==================================================

  // 记录启动时间
  startTime = millis();

  // ========== 摄像头初始化 ==========
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
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
  config.frame_size = FRAMESIZE_VGA;   // 视频流用 VGA，拍照时切 UXGA
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  if(psramFound()){
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    led_set_status(LED_CAMERA_ERROR);
    return;
  }

  led_set_status(LED_CAMERA_READY);

  sensor_t * s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_brightness(s, 1);
  s->set_saturation(s, 0);

  // ========== WiFi AP 模式 ==========
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32-CAM-Setup", "12345678");
  delay(500);
  Serial.println("==================== AP 模式 ====================");
  Serial.print("SSID: ESP32-CAM-Setup\nPassword: 12345678\nIP: ");
  Serial.println(WiFi.softAPIP());
  Serial.println("=================================================");

  // ========== NTP 时间同步（AP模式无互联网，已注释） ==========
  // configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  // 其他 NTP 相关代码全部注释
  // ==========================================================

  // 初始化SD卡
  Serial.println("Initializing SD card...");
  if(!sdmmcInit()){
    Serial.println("SD card initialization failed");
    led_set_status(LED_SD_ERROR);
  } else {
    Serial.println("SD card initialized successfully");
  }

  initPhotoDir();

  Serial.println("Cleaning up invalid video files...");
  int cleanedFiles = cleanInvalidVideoFiles();
  if(cleanedFiles > 0){
    Serial.printf("Cleaned up %d invalid video file(s)\n", cleanedFiles);
  }

  // 启动视频录制
  Serial.println("Starting video recording...");
  if(startVideoRecording(20, 1024, 768)){
    Serial.println("Video recording started successfully");
    int *fpsParam = (int*)malloc(sizeof(int));
    *fpsParam = 20;
    xTaskCreate(videoRecordTask, "video_record", 4096, fpsParam, 5, NULL);
  } else {
    Serial.println("Failed to start video recording");
  }

  startCameraServer();

  Serial.print("Camera Ready! Open 'http://");
  Serial.print(WiFi.softAPIP());
  Serial.println("' to connect");
}

// 视频录制任务
void videoRecordTask(void *pvParameters) {
  camera_fb_t *fb = NULL;
  int fps = *((int*)pvParameters);
  int delayMs = 1000 / fps;

  Serial.printf("Video recording task started, FPS: %d\n", fps);

  while(isRecordingVideo()) {
    fb = esp_camera_fb_get();
    if(!fb) {
      vTaskDelay(pdMS_TO_TICKS(delayMs));
      continue;
    }
    if(!writeVideoFrame(fb->buf, fb->len)) {
      Serial.println("Failed to write video frame");
    }
    esp_camera_fb_return(fb);
    vTaskDelay(pdMS_TO_TICKS(delayMs));
  }

  Serial.println("Video recording task stopped");
  free(pvParameters);
  vTaskDelete(NULL);
}

void loop() {
  delay(10000);

  static unsigned long last_cleanup = 0;
  unsigned long current_time = millis();
  if(current_time - last_cleanup >= 3600000) {
    uint32_t cleaned = auth_cleanup_expired_sessions();
    if(cleaned > 0) {
      Serial.printf("Cleaned up %u expired sessions\n", cleaned);
    }
    last_cleanup = current_time;
  }
}
