#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "app_tasks.h"
#include "app_types.h"
#include "wave_protocol.h"

namespace {
// StatusTask exclusively owns these buffers and all application serial output.
DisplayFrame telemetry;
uint8_t packet[WaveProtocol::MaxPacketBytes];
const char *stateName(AdcState state) {
  switch (state) {
    case AdcState::Starting: return "starting";
    case AdcState::Running: return "running";
    case AdcState::Timeout: return "timeout";
    case AdcState::Recovering: return "recovering";
    case AdcState::Error: return "error";
  }
  return "unknown";
}
}  // namespace

void StatusTask(void *argument) {
  auto &app = *static_cast<AppContext *>(argument);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  Serial.begin(AppConfig::SerialBaud);
  AdcStats adc;
  DisplayStats display;
  uint32_t previousSamples = 0;
  uint32_t previousFrames = 0;
  int64_t previousTime = esp_timer_get_time();
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(AppConfig::TelemetryPeriodMs));
    if (xQueuePeek(app.telemetryQueue, &telemetry, 0) == pdTRUE) {
      const size_t size = WaveProtocol::encode(telemetry, millis(), packet, sizeof(packet));
      Serial.write(packet, size);
    }
    if (xTaskGetTickCount() - lastWake >= pdMS_TO_TICKS(AppConfig::TelemetryPeriodMs)) {
      lastWake = xTaskGetTickCount();
    }
    if (esp_timer_get_time() - previousTime < AppConfig::StatusPeriodMs * 1000LL) continue;
    xQueuePeek(app.adcStatsQueue, &adc, 0);
    xQueuePeek(app.displayStatsQueue, &display, 0);
    const int64_t now = esp_timer_get_time();
    const double seconds = (now - previousTime) / 1000000.0;
    const double samplesPerSecond = static_cast<uint32_t>(adc.samples - previousSamples) / seconds;
    const double fps = static_cast<uint32_t>(display.frames - previousFrames) / seconds;
    previousSamples = adc.samples;
    previousFrames = display.frames;
    previousTime = now;

    Serial.printf("ADC: %.1f samples/s (target %lu) | FPS: %.1f | state=%s | calibrated=%s\n",
        samplesPerSecond, static_cast<unsigned long>(AppConfig::SampleRateHz), fps,
        stateName(adc.state), adc.calibrated ? "yes" : "no");
    Serial.printf("DMA: overflow=%lu timeout=%lu errors=%lu invalid=%lu restarts=%lu | skipped UI=%lu\n",
        static_cast<unsigned long>(adc.overflows), static_cast<unsigned long>(adc.timeouts),
        static_cast<unsigned long>(adc.readErrors), static_cast<unsigned long>(adc.invalidSamples),
        static_cast<unsigned long>(adc.restarts), static_cast<unsigned long>(display.skippedUpdates));
    Serial.printf("Max work: ADC=%lu us draw=%lu us | heartbeat age: ADC=%lu ms UI=%lu ms\n",
        static_cast<unsigned long>(adc.maxProcessUs), static_cast<unsigned long>(display.maxDrawUs),
        static_cast<unsigned long>(millis() - adc.heartbeatMs),
        static_cast<unsigned long>(millis() - display.heartbeatMs));
    Serial.printf("Internal heap: free=%u min=%u B | stack free: ADC=%u UI=%u status=%u B\n",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(app.adcTask)),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(app.displayTask)),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    Serial.printf("Flash=%lu B | PSRAM=%lu B free=%lu B | last ADC=%s UI=%s\n\n",
        static_cast<unsigned long>(ESP.getFlashChipSize()),
        static_cast<unsigned long>(ESP.getPsramSize()), static_cast<unsigned long>(ESP.getFreePsram()),
        esp_err_to_name(adc.lastError), esp_err_to_name(display.lastError));
    // 即使串口短时阻塞，也不忙循环追赶旧的输出周期。
    if (xTaskGetTickCount() - lastWake >= pdMS_TO_TICKS(AppConfig::TelemetryPeriodMs)) {
      lastWake = xTaskGetTickCount();
    }
  }
}
