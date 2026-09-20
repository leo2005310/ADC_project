#include <Arduino.h>
#include "app_config.h"
#include "app_tasks.h"
#include "app_types.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Select an ESP32-S3 board before compiling this program."
#endif

namespace {
AppContext app;

[[noreturn]] void startupFailed() {
  // 所有任务仍在等待启动通知，可安全清理；StatusTask 尚未使用串口。
  if (app.adcTask) vTaskDelete(app.adcTask);
  if (app.displayTask) vTaskDelete(app.displayTask);
  if (app.statusTask) vTaskDelete(app.statusTask);
  if (app.displayQueue) vQueueDelete(app.displayQueue);
  if (app.telemetryQueue) vQueueDelete(app.telemetryQueue);
  if (app.adcStatsQueue) vQueueDelete(app.adcStatsQueue);
  if (app.displayStatsQueue) vQueueDelete(app.displayStatsQueue);
  Serial.begin(AppConfig::SerialBaud);
  for (;;) {
    Serial.println("Startup failed: cannot allocate RTOS queues/tasks.");
    delay(1000);
  }
}
}  // namespace

void setup() {
  app.displayQueue = xQueueCreate(1, sizeof(DisplayFrame));
  app.telemetryQueue = xQueueCreate(1, sizeof(DisplayFrame));
  app.adcStatsQueue = xQueueCreate(1, sizeof(AdcStats));
  app.displayStatsQueue = xQueueCreate(1, sizeof(DisplayStats));
  if (!app.displayQueue || !app.telemetryQueue || !app.adcStatsQueue || !app.displayStatsQueue) startupFailed();

  if (xTaskCreatePinnedToCore(ADCProcessTask, "ADCProcessTask", AppConfig::AdcStackBytes,
                              &app, AppConfig::AdcPriority, &app.adcTask, AppConfig::AdcCore) != pdPASS ||
      xTaskCreatePinnedToCore(DisplayTask, "DisplayTask", AppConfig::DisplayStackBytes,
                              &app, AppConfig::DisplayPriority, &app.displayTask, AppConfig::DisplayCore) != pdPASS ||
      xTaskCreatePinnedToCore(StatusTask, "StatusTask", AppConfig::StatusStackBytes,
                              &app, AppConfig::StatusPriority, &app.statusTask, AppConfig::StatusCore) != pdPASS) {
    startupFailed();
  }
  // 句柄全部就绪后放行，避免状态任务读到尚未创建的任务句柄。
  xTaskNotifyGive(app.statusTask);
  xTaskNotifyGive(app.displayTask);
  xTaskNotifyGive(app.adcTask);
}

void loop() {
  // Arduino loopTask 保留但阻塞，业务由三个独立任务负责。
  vTaskDelay(portMAX_DELAY);
}
