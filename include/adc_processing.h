#pragma once
#include "app_types.h"

// 与硬件和 RTOS 无关，便于在电脑上验证滤波、分块和波形顺序。
class AdcProcessor {
 public:
  void reset();
  bool addSample(uint16_t raw, uint16_t milliVolts);
  void snapshot(DisplayFrame &frame) const;
 private:
  uint32_t rawSum_ = 0;
  uint32_t mvSum_ = 0;
  size_t blockCount_ = 0;
  uint16_t rawMean_ = 0;
  float filteredMv_ = 0;
  bool filterReady_ = false;
  bool blockNearLimit_ = false;
  bool nearLimit_ = false;
  size_t bucketCount_ = 0;
  uint16_t bucketMin_ = UINT16_MAX;
  uint16_t bucketMax_ = 0;
  uint16_t waveMin_[AppConfig::WaveColumns] = {};
  uint16_t waveMax_[AppConfig::WaveColumns] = {};
  size_t waveNext_ = 0;
  size_t waveCount_ = 0;
};
