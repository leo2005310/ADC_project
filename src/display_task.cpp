#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <esp_timer.h>

#include "app_tasks.h"
#include "app_types.h"

namespace {
using namespace AppConfig;
constexpr uint16_t Background = ST77XX_BLACK;
constexpr uint16_t Muted = 0x8410;
constexpr uint16_t Grid = 0x2104;

void drawStaticScreen(Adafruit_ST7789 &tft) {
  tft.fillScreen(Background);
  tft.setTextWrap(false);
  tft.setTextColor(ST77XX_WHITE, Background);
  tft.setTextSize(2);
  tft.setCursor(12, 8);
  tft.print("ESP32-S3 ADC");
  tft.setTextSize(1);
  tft.setTextColor(Muted, Background);
  tft.setCursor(12, 32);
  tft.printf("DMA %lu Hz | GPIO1", static_cast<unsigned long>(SampleRateHz));
  tft.setCursor(12, 92);
  tft.print("Voltage / 0 - 3.1 V");
  tft.drawRect(PlotX - 1, PlotY - 1, WaveColumns + 2, PlotHeight + 2, Muted);
  tft.setCursor(12, 197);
  tft.printf("<-- %.2f s history     now", WaveColumns * WaveColumnMs / 1000.0);
  tft.drawRect(12, 225, 216, 10, Muted);
}

int16_t voltageY(uint16_t mv) {
  const uint32_t clipped = mv > PlotMaxMv ? PlotMaxMv : mv;
  return PlotHeight - 1 - clipped * (PlotHeight - 1) / PlotMaxMv;
}

const char *stateText(const DisplayFrame &frame, bool stale) {
  if (frame.state == AdcState::Error) return "ADC ERROR: see serial";
  if (frame.state == AdcState::Timeout) return "ADC TIMEOUT";
  if (frame.state == AdcState::Recovering) return "ADC RECOVERING";
  if (stale) return "ADC DATA STALE";
  if (!frame.valid) return "WAITING FOR ADC";
  return frame.nearLimit ? "NEAR LIMIT: may clip" : "ADC RUNNING";
}

void drawFrame(Adafruit_ST7789 &tft, GFXcanvas16 &waveform,
               const DisplayFrame &frame, bool stale) {
  char text[32];
  const bool showValue = frame.valid && !stale && frame.state == AdcState::Running;
  tft.setTextSize(3);
  tft.setTextColor(showValue ? ST77XX_CYAN : Muted, Background);
  tft.setCursor(12, 48);
  if (showValue) snprintf(text, sizeof(text), "%5.3f V", frame.milliVolts / 1000.0);
  else snprintf(text, sizeof(text), "--.--- V");
  // 字段固定清理到 8 个字符，避免错误状态恢复后残留一个 V。
  tft.fillRect(12, 48, 8 * 18, 24, Background);
  tft.print(text);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, Background);
  tft.setCursor(12, 79);
  if (showValue) snprintf(text, sizeof(text), "RAW: %4u / 4095", frame.rawMean);
  else snprintf(text, sizeof(text), "RAW: ---- / 4095");
  tft.print(text);

  waveform.fillScreen(Background);
  for (int16_t y = 0; y < PlotHeight; y += PlotHeight / 3) {
    waveform.drawFastHLine(0, y, WaveColumns, Grid);
  }
  for (size_t x = 0; x < WaveColumns; x += 54) {
    waveform.drawFastVLine(x, 0, PlotHeight, Grid);
  }
  const size_t count = frame.waveCount <= WaveColumns ? frame.waveCount : WaveColumns;
  if (showValue) {
    for (size_t i = 0; i < count; ++i) {
      const int16_t x = WaveColumns - count + i;
      const int16_t top = voltageY(frame.waveMax[i]);
      const int16_t bottom = voltageY(frame.waveMin[i]);
      waveform.drawFastVLine(x, top, bottom - top + 1, ST77XX_CYAN);
    }
  }
  tft.drawRGBBitmap(PlotX, PlotY, waveform.getBuffer(), WaveColumns, PlotHeight);
  tft.setCursor(12, 212);
  tft.setTextColor(showValue && !frame.nearLimit ? ST77XX_GREEN : ST77XX_YELLOW, Background);
  tft.fillRect(12, 212, 216, 8, Background);
  tft.print(stateText(frame, stale));
  const int16_t filled = showValue ? static_cast<uint32_t>(frame.rawMean) * 212 / AdcMax : 0;
  if (filled > 0) tft.fillRect(14, 227, filled, 6, ST77XX_CYAN);
  if (filled < 212) tft.fillRect(14 + filled, 227, 212 - filled, 6, Background);
}
}  // namespace

void DisplayTask(void *argument) {
  auto &app = *static_cast<AppContext *>(argument);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  pinMode(AppConfig::TftBacklight, OUTPUT);
  digitalWrite(AppConfig::TftBacklight, LOW);
  Adafruit_ST7789 tft(&SPI, AppConfig::TftCs, AppConfig::TftDc, AppConfig::TftReset);
  SPI.begin(AppConfig::TftClock, -1, AppConfig::TftMosi, AppConfig::TftCs);
  tft.init(240, 240);
  tft.setSPISpeed(AppConfig::SpiHz);
  tft.setRotation(AppConfig::TftRotation);
  drawStaticScreen(tft);
  digitalWrite(AppConfig::TftBacklight, HIGH);  // 屏幕初始化完成后开启背光。
  GFXcanvas16 waveform(AppConfig::WaveColumns, AppConfig::PlotHeight);
  DisplayFrame latest;
  DisplayStats stats;
  if (!waveform.getBuffer()) {
    tft.setCursor(12, 48);
    tft.print("NO WAVE BUFFER");
    stats.lastError = ESP_ERR_NO_MEM;
    for (;;) {
      stats.heartbeatMs = millis();
      xQueueOverwrite(app.displayStatsQueue, &stats);
      vTaskDelay(pdMS_TO_TICKS(AppConfig::StatusPeriodMs));
    }
  }

  bool firstDraw = true;
  bool lastStale = false;
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    const uint32_t previousSequence = latest.sequence;
    const bool received = xQueueReceive(app.displayQueue, &latest, 0) == pdTRUE;
    const bool stale = latest.valid &&
        static_cast<uint32_t>(millis() - latest.timestampMs) > AppConfig::StaleDataMs;
    if (received && previousSequence != 0) {
      const uint32_t delta = latest.sequence - previousSequence;
      if (delta > 1) stats.skippedUpdates += delta - 1;
    }
    if (received || firstDraw || stale != lastStale) {
      const int64_t start = esp_timer_get_time();
      drawFrame(tft, waveform, latest, stale);
      const uint32_t drawUs = esp_timer_get_time() - start;
      if (drawUs > stats.maxDrawUs) stats.maxDrawUs = drawUs;
      // 只统计新有效数据完成的绘制，不将等待界面计入数据 FPS。
      if (received && latest.valid && latest.state == AdcState::Running && !stale) ++stats.frames;
      firstDraw = false;
      lastStale = stale;
    }
    stats.heartbeatMs = millis();
    xQueueOverwrite(app.displayStatsQueue, &stats);
    if (xTaskGetTickCount() - lastWake >= pdMS_TO_TICKS(AppConfig::DisplayPeriodMs)) {
      lastWake = xTaskGetTickCount();  // 超时后不连续追赶旧的刷新周期。
    }
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(AppConfig::DisplayPeriodMs));
  }
}
