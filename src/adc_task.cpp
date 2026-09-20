#include <Arduino.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>
#include <esp_timer.h>
#include <string.h>

#include "adc_processing.h"
#include "app_tasks.h"

namespace {
using namespace AppConfig;
static_assert(ReadBufferBytes % SOC_ADC_DIGI_RESULT_BYTES == 0, "Unaligned ADC frame");
static_assert(sizeof(adc_digi_output_data_t) == SampleBytes, "Unexpected ADC format");
static_assert(SampleRateHz >= SOC_ADC_SAMPLE_FREQ_THRES_LOW &&
              SampleRateHz <= SOC_ADC_SAMPLE_FREQ_THRES_HIGH, "Unsupported sample rate");

// 由 ADCProcessTask 独占的 A/B 读取缓冲，交替复用。
// DMA 实际写入驱动内部缓冲；read_bytes 将数据复制到这里，并非直接 DMA 双缓冲。
alignas(4) uint8_t readBuffers[2][ReadBufferBytes];
AdcProcessor processor;
DisplayFrame frame;
AdcStats stats;
esp_adc_cal_characteristics_t calibration;
bool initialized = false;
bool running = false;

esp_err_t closeDriver() {
  if (running) {
    const esp_err_t error = adc_digi_stop();
    if (error != ESP_OK) return error;
    running = false;
  }
  if (initialized) {
    const esp_err_t error = adc_digi_deinitialize();
    if (error != ESP_OK) return error;
    initialized = false;
  }
  return ESP_OK;
}

esp_err_t startDriver() {
  adc_digi_init_config_t dma = {};
  dma.max_store_buf_size = DriverBufferBytes;
  dma.conv_num_each_intr = ReadBufferBytes;  // 单位是字节，不是采样点。
  dma.adc1_chan_mask = 1U << AdcChannel;
  esp_err_t error = adc_digi_initialize(&dma);
  if (error != ESP_OK) return error;
  initialized = true;

  adc_digi_pattern_config_t pattern = {};
  pattern.atten = ADC_ATTEN_DB_12;
  pattern.channel = AdcChannel;
  pattern.unit = 0;  // 此旧版 DMA pattern 使用 0 表示 ADC1，而 ADC_UNIT_1 枚举为 1。
  pattern.bit_width = 12;
  adc_digi_configuration_t config = {};
  config.conv_limit_en = false;
  config.conv_limit_num = 250;
  config.pattern_num = 1;
  config.adc_pattern = &pattern;
  config.sample_freq_hz = SampleRateHz;
  config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
  error = adc_digi_controller_configure(&config);
  if (error != ESP_OK) return error;
  error = adc_digi_start();
  running = error == ESP_OK;
  return error;
}

void publishStats(AppContext &app) {
  stats.heartbeatMs = millis();
  xQueueOverwrite(app.adcStatsQueue, &stats);
}

void publishFrame(AppContext &app, AdcState state) {
  processor.snapshot(frame);
  frame.state = state;
  frame.timestampMs = millis();
  ++frame.sequence;
  stats.state = state;
  xQueueOverwrite(app.displayQueue, &frame);
}

[[noreturn]] void parkWithError(AppContext &app, esp_err_t error) {
  processor.reset();
  stats.lastError = error;
  for (;;) {
    publishFrame(app, AdcState::Error);
    publishStats(app);
    vTaskDelay(pdMS_TO_TICKS(StatusPeriodMs));
  }
}
}  // namespace

void ADCProcessTask(void *argument) {
  auto &app = *static_cast<AppContext *>(argument);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  pinMode(AppConfig::PotPin, INPUT);
  publishFrame(app, AdcState::Starting);

  // ESP32-S3 使用 eFuse TP fitting 校准。没有校准数据时明确报错，不伪造电压。
  if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP_FIT) != ESP_OK) {
    parkWithError(app, ESP_ERR_NOT_SUPPORTED);
  }
  stats.calibrated = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12,
      ADC_WIDTH_BIT_12, 1100, &calibration) == ESP_ADC_CAL_VAL_EFUSE_TP_FIT;
  if (!stats.calibrated) parkWithError(app, ESP_ERR_NOT_SUPPORTED);

  uint8_t bufferIndex = 0;
  bool hasStarted = false;
  for (;;) {
    if (!running) {
      const esp_err_t cleanup = closeDriver();
      if (cleanup != ESP_OK) parkWithError(app, cleanup);
      const esp_err_t error = startDriver();
      if (error != ESP_OK) {
        stats.lastError = error;
        publishFrame(app, AdcState::Error);
        publishStats(app);
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      if (hasStarted) ++stats.restarts;
      hasStarted = true;
      processor.reset();
      stats.lastError = ESP_OK;
    }

    uint32_t bytesRead = 0;
    uint8_t *buffer = readBuffers[bufferIndex];
    const esp_err_t result = adc_digi_read_bytes(buffer, ReadBufferBytes,
                                                &bytesRead, ReadTimeoutMs);
    bufferIndex ^= 1;
    if (result != ESP_OK) {
      stats.lastError = result;
      AdcState state = AdcState::Recovering;
      if (result == ESP_ERR_TIMEOUT) {
        ++stats.timeouts;
        state = AdcState::Timeout;
      } else if (result == ESP_ERR_INVALID_STATE) {
        ++stats.overflows;
      } else {
        ++stats.readErrors;
      }
      // 4.4 驱动的溢出标志会保持到重启。重建驱动还会丢弃缓存中的旧数据。
      // 此处计数是溢出事件数，无法由旧驱动获知精确丢失样本数。
      const esp_err_t cleanup = closeDriver();
      processor.reset();  // 断流后重建时间轴，避免将不连续数据画成连续波形。
      publishFrame(app, state);
      publishStats(app);
      if (cleanup != ESP_OK) parkWithError(app, cleanup);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    const int64_t processStart = esp_timer_get_time();
    if (bytesRead > ReadBufferBytes || bytesRead % SampleBytes != 0) {
      ++stats.readErrors;
      stats.lastError = ESP_ERR_INVALID_SIZE;
      processor.reset();
      publishFrame(app, AdcState::Error);
    } else {
      // 实际读取长度可小于 512 字节。处理器跨读取累积到 128 个有效样本。
      for (size_t offset = 0; offset < bytesRead; offset += SampleBytes) {
        adc_digi_output_data_t sample;
        memcpy(&sample, buffer + offset, sizeof(sample));
        if (sample.type2.unit != 0 || sample.type2.channel != AdcChannel) {
          ++stats.invalidSamples;
          processor.reset();
          publishFrame(app, AdcState::Recovering);
          continue;
        }
        ++stats.samples;
        const uint16_t raw = sample.type2.data;
        const uint32_t mv = esp_adc_cal_raw_to_voltage(raw, &calibration);
        if (processor.addSample(raw, static_cast<uint16_t>(mv > UINT16_MAX ? UINT16_MAX : mv))) {
          ++stats.blocks;
          stats.lastError = ESP_OK;
          publishFrame(app, AdcState::Running);
        }
      }
    }
    const uint32_t processUs = esp_timer_get_time() - processStart;
    if (processUs > stats.maxProcessUs) stats.maxProcessUs = processUs;
    publishStats(app);
    // 通常 read_bytes 已阻塞约 32ms；积压时也给 Core 0 Idle 留出执行机会。
    vTaskDelay(1);
  }
}
