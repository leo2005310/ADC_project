#include "adc_processing.h"

void AdcProcessor::reset() {
  rawSum_ = mvSum_ = 0;
  blockCount_ = bucketCount_ = waveNext_ = waveCount_ = 0;
  rawMean_ = bucketMax_ = 0;
  bucketMin_ = UINT16_MAX;
  filteredMv_ = 0;
  filterReady_ = blockNearLimit_ = nearLimit_ = false;
  // 历史数组由 waveCount_ 控制可见性，无需在每次断流时清空。
}

bool AdcProcessor::addSample(uint16_t raw, uint16_t milliVolts) {
  rawSum_ += raw;
  mvSum_ += milliVolts;
  blockNearLimit_ |= raw >= 4000 || milliVolts >= 3050;
  if (milliVolts < bucketMin_) bucketMin_ = milliVolts;
  if (milliVolts > bucketMax_) bucketMax_ = milliVolts;
  if (++bucketCount_ == AppConfig::SamplesPerColumn) {
    waveMin_[waveNext_] = bucketMin_;
    waveMax_[waveNext_] = bucketMax_;
    waveNext_ = (waveNext_ + 1) % AppConfig::WaveColumns;
    if (waveCount_ < AppConfig::WaveColumns) ++waveCount_;
    bucketCount_ = 0;
    bucketMin_ = UINT16_MAX;
    bucketMax_ = 0;
  }
  if (++blockCount_ != AppConfig::BlockSamples) return false;
  rawMean_ = (rawSum_ + AppConfig::BlockSamples / 2) / AppConfig::BlockSamples;
  const float meanMv = static_cast<float>(mvSum_) / AppConfig::BlockSamples;
  filteredMv_ = filterReady_
                    ? filteredMv_ + AppConfig::VoltageFilterAlpha * (meanMv - filteredMv_)
                    : meanMv;
  filterReady_ = true;
  nearLimit_ = blockNearLimit_;
  blockNearLimit_ = false;
  rawSum_ = mvSum_ = 0;
  blockCount_ = 0;
  return true;
}

void AdcProcessor::snapshot(DisplayFrame &frame) const {
  frame.rawMean = rawMean_;
  frame.milliVolts = static_cast<uint16_t>(filteredMv_ + 0.5f);
  frame.valid = filterReady_;
  frame.nearLimit = nearLimit_;
  frame.waveCount = waveCount_;
  // 输出按时间排序的独立快照；未填满时由显示任务在右侧对齐。
  const size_t oldest = (waveNext_ + AppConfig::WaveColumns - waveCount_) % AppConfig::WaveColumns;
  for (size_t i = 0; i < AppConfig::WaveColumns; ++i) {
    const size_t index = (oldest + i) % AppConfig::WaveColumns;
    frame.waveMin[i] = i < waveCount_ ? waveMin_[index] : 0;
    frame.waveMax[i] = i < waveCount_ ? waveMax_[index] : 0;
  }
}
