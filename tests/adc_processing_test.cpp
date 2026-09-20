#include <assert.h>
#include <stdio.h>
#include "adc_processing.h"

static void constantAndStep() {
  AdcProcessor processor;
  DisplayFrame frame;
  processor.snapshot(frame);
  assert(!frame.valid && frame.waveCount == 0);
  for (size_t i = 0; i < AppConfig::BlockSamples; ++i) {
    assert(processor.addSample(1500, 1000) == (i + 1 == AppConfig::BlockSamples));
  }
  processor.snapshot(frame);
  assert(frame.valid && frame.rawMean == 1500 && frame.milliVolts == 1000);
  assert(frame.waveCount == 3);
  for (size_t i = 0; i < AppConfig::BlockSamples; ++i) processor.addSample(2500, 2000);
  processor.snapshot(frame);
  assert(frame.rawMean == 2500 && frame.milliVolts == 1200);
  // 包含第 128 个样本处阶跃的同一波形桶必须同时保留两个极值。
  assert(frame.waveMin[3] == 1000 && frame.waveMax[3] == 2000);
}

static void chunkBoundaries() {
  AdcProcessor processor;
  const size_t chunks[] = {1, 7, 31, 3, 120, 17, 128, 19};
  size_t total = 0;
  size_t blocks = 0;
  for (size_t repeat = 0; repeat < 9; ++repeat) {
    for (size_t chunk : chunks) {
      for (size_t i = 0; i < chunk; ++i) {
        if (processor.addSample(1024, 750)) ++blocks;
        ++total;
      }
    }
  }
  DisplayFrame frame;
  processor.snapshot(frame);
  assert(blocks == total / AppConfig::BlockSamples);
  assert(frame.rawMean == 1024 && frame.milliVolts == 750);
  assert(frame.waveCount == total / AppConfig::SamplesPerColumn);
}

static void waveformWrapAndSnapshot() {
  AdcProcessor processor;
  constexpr size_t groups = AppConfig::WaveColumns + 5;
  for (size_t group = 0; group < groups; ++group) {
    for (size_t i = 0; i < AppConfig::SamplesPerColumn; ++i) {
      processor.addSample(1000, 50 + group * 2 + (i % 2));
    }
  }
  DisplayFrame frame;
  processor.snapshot(frame);
  assert(frame.waveCount == AppConfig::WaveColumns);
  for (size_t i = 0; i < AppConfig::WaveColumns; ++i) {
    assert(frame.waveMin[i] == 50 + (i + 5) * 2);
    assert(frame.waveMax[i] == 51 + (i + 5) * 2);
  }
  // 已复制出的显示快照不能随下一批采样变化。
  for (size_t i = 0; i < 1000; ++i) processor.addSample(4095, 3100);
  assert(frame.waveMin[0] == 60 && frame.waveMax[0] == 61);
}

static void saturationAndRecovery() {
  AdcProcessor processor;
  DisplayFrame frame;
  for (size_t i = 0; i < AppConfig::BlockSamples; ++i) {
    processor.addSample(i == 0 ? 4095 : 0, i == 0 ? 3100 : 0);
  }
  processor.snapshot(frame);
  assert(frame.nearLimit && frame.rawMean == 32);
  assert(frame.waveMin[0] == 0 && frame.waveMax[0] == 3100);
  for (size_t i = 0; i < AppConfig::BlockSamples; ++i) processor.addSample(0, 0);
  processor.snapshot(frame);
  assert(!frame.nearLimit);
  for (size_t i = 0; i < 17; ++i) processor.addSample(4095, 3100);
  processor.reset();  // 模拟断流，不能继承旧的半块、滤波状态或波形。
  processor.snapshot(frame);
  assert(!frame.valid && frame.waveCount == 0 && !frame.nearLimit);
  for (size_t i = 0; i < AppConfig::BlockSamples; ++i) processor.addSample(200, 150);
  processor.snapshot(frame);
  assert(frame.rawMean == 200 && frame.milliVolts == 150);
  assert(frame.waveCount == 3 && frame.waveMin[0] == 150 && frame.waveMax[0] == 150);
}

int main() {
  constantAndStep();
  chunkBoundaries();
  waveformWrapAndSnapshot();
  saturationAndRecovery();
  puts("PASS: DC/step, chunk boundaries, waveform wrap/snapshot, saturation/recovery");
}
