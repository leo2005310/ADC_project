#pragma once
#include "app_config.h"

enum class AdcState : uint8_t { Starting, Running, Timeout, Recovering, Error };

struct DisplayFrame {
  uint32_t sequence = 0;
  uint32_t timestampMs = 0;
  uint16_t rawMean = 0;
  uint16_t milliVolts = 0;
  uint16_t waveCount = 0;
  uint16_t waveMin[AppConfig::WaveColumns] = {};
  uint16_t waveMax[AppConfig::WaveColumns] = {};
  AdcState state = AdcState::Starting;
  bool valid = false;
  bool nearLimit = false;
};

struct AdcStats {
  uint32_t samples = 0;
  uint32_t blocks = 0;
  uint32_t overflows = 0;
  uint32_t timeouts = 0;
  uint32_t readErrors = 0;
  uint32_t invalidSamples = 0;
  uint32_t restarts = 0;
  uint32_t maxProcessUs = 0;
  uint32_t heartbeatMs = 0;
  int32_t lastError = 0;
  AdcState state = AdcState::Starting;
  bool calibrated = false;
};

struct DisplayStats {
  uint32_t frames = 0;
  uint32_t skippedUpdates = 0;
  uint32_t maxDrawUs = 0;
  uint32_t heartbeatMs = 0;
  int32_t lastError = 0;
};
