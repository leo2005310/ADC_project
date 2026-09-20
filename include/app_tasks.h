#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

struct AppContext {
  QueueHandle_t displayQueue = nullptr;
  QueueHandle_t telemetryQueue = nullptr;
  QueueHandle_t adcStatsQueue = nullptr;
  QueueHandle_t displayStatsQueue = nullptr;
  TaskHandle_t adcTask = nullptr;
  TaskHandle_t displayTask = nullptr;
  TaskHandle_t statusTask = nullptr;
};
void ADCProcessTask(void *argument);
void DisplayTask(void *argument);
void StatusTask(void *argument);
